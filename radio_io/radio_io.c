/* HERMES Modem - PTT-oriented radio I/O abstraction
 *
 * Copyright (C) 2025 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3, or (at your option)
 * any later version.
 *
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HAVE_HAMLIB
#include <hamlib/rig.h>
#include "rigctl_parse.h"
#endif

#include "radio_io.h"
#include "radio_port.h"
#include "serial_ptt.h"
#include "cm108_ptt.h"
#include "../common/hermes_log.h"

#define RADIO_LOG_TAG "radio-io"

#ifdef HAVE_HERMES_SHM
#include "sbitx_io.h"
#include "radio_cmds.h"
#include "shm_utils.h"
#endif

typedef struct {
    int  (*open)(const ptt_config_t *config);
    int  (*set)(bool on);
    void (*close)(void);
} ptt_backend_t;

/* One process owns one PTT endpoint.  The mutex serializes UI restarts against
 * modem keying and also protects the selected config/backend pointer. */
static pthread_mutex_t g_radio_mutex = PTHREAD_MUTEX_INITIALIZER;
static ptt_config_t g_config = {
    .method = PTT_METHOD_NONE,
    .device = {0},
    .hamlib_model = RADIO_TYPE_NONE,
    .hamlib_serial_speed = 0,
    .hamlib_log_level = 0
};
static const ptt_backend_t *g_backend = NULL;

static const char *ptt_method_name(ptt_method_t method)
{
    switch (method)
    {
    case PTT_METHOD_NONE:        return "none";
    case PTT_METHOD_HAMLIB:      return "hamlib";
    case PTT_METHOD_SERIAL:      return "serial";
    case PTT_METHOD_CM108:       return "cm108";
    case PTT_METHOD_HERMES_SHM:  return "hermes_shm";
    default:                     return "unknown";
    }
}

/* ------------------------------------------------------------------------- */
/* Hamlib backend                                                            */

#ifdef HAVE_HAMLIB
static RIG *g_radio = NULL;

static radio_port_kind_t radio_io_port_kind(rig_model_t model)
{
    switch ((rig_port_t)rig_get_caps_int(model, RIG_CAPS_PORT_TYPE))
    {
    case RIG_PORT_SERIAL:
        return RADIO_PORT_KIND_SERIAL;
    case RIG_PORT_NETWORK:
    case RIG_PORT_UDP_NETWORK:
        return RADIO_PORT_KIND_NETWORK;
    default:
        return RADIO_PORT_KIND_OTHER;
    }
}

static const char *radio_io_model_name(rig_model_t model)
{
    const char *name = rig_get_caps_cptr(model, RIG_CAPS_MODEL_NAME_CPTR);
    return name ? name : "this rig";
}

static void radio_io_apply_hamlib_conf(RIG *radio, rig_model_t model,
                                       const char *device_path,
                                       int serial_speed)
{
    int rc;

    if (device_path && device_path[0])
    {
        rc = rig_set_conf(radio, rig_token_lookup(radio, "rig_pathname"),
                          device_path);
        if (rc != RIG_OK)
            HLOGW(RADIO_LOG_TAG, "rig_set_conf(rig_pathname) failed: %d", rc);
    }

    if (serial_speed <= 0)
        return;

    if (radio_io_port_kind(model) != RADIO_PORT_KIND_SERIAL)
    {
        HLOGI(RADIO_LOG_TAG,
              "hamlib_serial_speed=%d ignored: %s is not a serial rig",
              serial_speed, radio_io_model_name(model));
        return;
    }

    char rate[16];
    snprintf(rate, sizeof(rate), "%d", serial_speed);
    rc = rig_set_conf(radio, rig_token_lookup(radio, "serial_speed"), rate);
    if (rc != RIG_OK)
        HLOGW(RADIO_LOG_TAG, "rig_set_conf(serial_speed) failed: %d", rc);
    else
        HLOGI(RADIO_LOG_TAG, "Hamlib serial speed overridden to %d baud",
              serial_speed);
}

static void radio_io_warn_port_mismatch(rig_model_t model,
                                        const char *device_path)
{
    radio_port_verdict_t verdict =
        radio_port_check(radio_io_port_kind(model), device_path);
    const char *advice = radio_port_advice(verdict);
    if (advice)
        HLOGW(RADIO_LOG_TAG, "PTT device '%s' looks wrong for %s -- %s",
              device_path, radio_io_model_name(model), advice);
}

static int hamlib_open(const ptt_config_t *config)
{
    if (config->hamlib_model <= 0)
    {
        HLOGE(RADIO_LOG_TAG, "Hamlib PTT requires a positive model ID");
        return -1;
    }

    rig_set_debug((enum rig_debug_level_e)config->hamlib_log_level);
    g_radio = rig_init(config->hamlib_model);
    if (!g_radio)
    {
        HLOGE(RADIO_LOG_TAG, "Unknown Hamlib rig model %d or initialization error",
              config->hamlib_model);
        HLOGE(RADIO_LOG_TAG, "Check available radios with -K");
        return -1;
    }

    radio_io_apply_hamlib_conf(g_radio, config->hamlib_model,
                               config->device,
                               config->hamlib_serial_speed);
    radio_io_warn_port_mismatch(config->hamlib_model, config->device);

    int ret = rig_open(g_radio);
    if (ret != RIG_OK)
    {
        HLOGE(RADIO_LOG_TAG, "rig_open(%s) failed: %s (%d)",
              config->device[0] ? config->device : "default",
              rigerror2(ret), ret);
        const char *advice = radio_port_advice(
            radio_port_check(radio_io_port_kind(config->hamlib_model),
                             config->device));
        if (advice)
            HLOGE(RADIO_LOG_TAG, "%s", advice);
        rig_cleanup(g_radio);
        g_radio = NULL;
        return -1;
    }

    if (g_radio->caps->rig_model == RIG_MODEL_NETRIGCTL)
    {
        int rc = rig_set_vfo_opt(g_radio, netrigctl_get_vfo_mode(g_radio));
        if (rc != RIG_OK)
            HLOGW(RADIO_LOG_TAG, "Failed to set NetRigCtl VFO option: %d", rc);
    }

    HLOGI(RADIO_LOG_TAG, "PTT method: Hamlib (model %d, device %s)",
          config->hamlib_model,
          config->device[0] ? config->device : "default");
    HLOGI(RADIO_LOG_TAG, "Hamlib runtime: %s",
          hamlib_version2 ? hamlib_version2 : "version unknown");
    return 0;
}

static int hamlib_set(bool on)
{
    if (!g_radio)
        return -1;
    int ret = rig_set_ptt(g_radio, RIG_VFO_CURR,
                          on ? RIG_PTT_ON : RIG_PTT_OFF);
    if (ret != RIG_OK)
    {
        HLOGW(RADIO_LOG_TAG, "PTT %s failed (Hamlib model %d): %s",
              on ? "ON" : "OFF", g_config.hamlib_model, rigerror(ret));
        return -1;
    }
    HLOGD(RADIO_LOG_TAG, "PTT %s via Hamlib (model %d)",
          on ? "ON" : "OFF", g_config.hamlib_model);
    return 0;
}

static void hamlib_close(void)
{
    if (!g_radio)
        return;
    (void)rig_set_ptt(g_radio, RIG_VFO_CURR, RIG_PTT_OFF);
    rig_close(g_radio);
    rig_cleanup(g_radio);
    g_radio = NULL;
}
#else
static int hamlib_open(const ptt_config_t *config)
{
    (void)config;
    HLOGE(RADIO_LOG_TAG,
          "Hamlib support not compiled in. Install libhamlib-dev and rebuild.");
    return -1;
}
static int hamlib_set(bool on) { (void)on; return -1; }
static void hamlib_close(void) { }
#endif

/* ------------------------------------------------------------------------- */
/* HERMES shared-memory backend                                               */

#ifdef HAVE_HERMES_SHM
static controller_conn *g_sbitx_connector = NULL;

static int shm_ptt_open(const ptt_config_t *config)
{
    (void)config;
    if (!shm_is_created(SYSV_SHM_CONTROLLER_KEY_STR, sizeof(controller_conn)))
    {
        HLOGE(RADIO_LOG_TAG,
              "Radio SHM not created. Is sbitx_controller running?");
        return -1;
    }
    g_sbitx_connector = (controller_conn *)shm_attach(
        SYSV_SHM_CONTROLLER_KEY_STR, sizeof(controller_conn));
    if (!g_sbitx_connector)
    {
        HLOGE(RADIO_LOG_TAG, "Failed to attach to radio SHM");
        return -1;
    }
    HLOGI(RADIO_LOG_TAG, "PTT method: HERMES shared memory");
    return 0;
}

static int shm_ptt_set(bool on)
{
    if (!g_sbitx_connector)
        return -1;
    uint8_t command[5] = {0};
    uint8_t response[5] = {0};
    command[4] = on ? CMD_PTT_ON : CMD_PTT_OFF;
    radio_cmd(g_sbitx_connector, command, response);
    HLOGD(RADIO_LOG_TAG, "PTT %s via HERMES SHM", on ? "ON" : "OFF");
    return 0;
}

static void shm_ptt_close(void)
{
    if (!g_sbitx_connector)
        return;
    (void)shm_ptt_set(false);
    shm_dettach(SYSV_SHM_CONTROLLER_KEY_STR,
                sizeof(controller_conn), g_sbitx_connector);
    g_sbitx_connector = NULL;
}
#else
static int shm_ptt_open(const ptt_config_t *config)
{
    (void)config;
    HLOGE(RADIO_LOG_TAG,
          "HERMES SHM PTT is only available on supported Linux builds");
    return -1;
}
static int shm_ptt_set(bool on) { (void)on; return -1; }
static void shm_ptt_close(void) { }
#endif

/* ------------------------------------------------------------------------- */
/* Serial modem-control-line backend                                         */

static const char *line_name(ptt_line_t line)
{
    switch (line)
    {
    case PTT_LINE_DTR:  return "DTR";
    case PTT_LINE_BOTH: return "RTS+DTR";
    case PTT_LINE_RTS:
    default:            return "RTS";
    }
}

static int serial_backend_open(const ptt_config_t *config)
{
    if (serial_ptt_open(config) != 0)
        return -1;
    HLOGI(RADIO_LOG_TAG, "PTT method: serial %s%s%s (device %s)",
          line_name(config->serial_line),
          (config->serial_invert_rts || config->serial_invert_dtr) ? ", inverted " : "",
          config->serial_invert_rts
              ? (config->serial_invert_dtr ? "RTS+DTR" : "RTS")
              : (config->serial_invert_dtr ? "DTR" : ""),
          config->device);
    return 0;
}

static int serial_backend_set(bool on)
{
    int rc = serial_ptt_set(on);
    if (rc == 0)
        HLOGD(RADIO_LOG_TAG, "PTT %s via serial", on ? "ON" : "OFF");
    return rc;
}

static void serial_backend_close(void)
{
    serial_ptt_close();
}

/* ------------------------------------------------------------------------- */
/* CM108 GPIO backend                                                        */

static int cm108_backend_open(const ptt_config_t *config)
{
    if (cm108_ptt_open(config) != 0)
        return -1;
    HLOGI(RADIO_LOG_TAG, "PTT method: CM108 GPIO%d", config->cm108_gpio);
    return 0;
}

static int cm108_backend_set(bool on)
{
    int rc = cm108_ptt_set(on);
    if (rc == 0)
        HLOGD(RADIO_LOG_TAG, "PTT %s via CM108 GPIO", on ? "ON" : "OFF");
    return rc;
}

static void cm108_backend_close(void)
{
    cm108_ptt_close();
}

static const ptt_backend_t HAMLIB_BACKEND = {
    hamlib_open, hamlib_set, hamlib_close
};
static const ptt_backend_t SERIAL_BACKEND = {
    serial_backend_open, serial_backend_set, serial_backend_close
};
static const ptt_backend_t CM108_BACKEND = {
    cm108_backend_open, cm108_backend_set, cm108_backend_close
};
static const ptt_backend_t HERMES_SHM_BACKEND = {
    shm_ptt_open, shm_ptt_set, shm_ptt_close
};

static const ptt_backend_t *backend_for_method(ptt_method_t method)
{
    switch (method)
    {
    case PTT_METHOD_HAMLIB:     return &HAMLIB_BACKEND;
    case PTT_METHOD_SERIAL:     return &SERIAL_BACKEND;
    case PTT_METHOD_CM108:      return &CM108_BACKEND;
    case PTT_METHOD_HERMES_SHM: return &HERMES_SHM_BACKEND;
    case PTT_METHOD_NONE:       return NULL;
    default:                    return NULL;
    }
}

static void radio_io_shutdown_locked(void)
{
    if (g_backend)
        g_backend->close();
    g_backend = NULL;
    g_config.method = PTT_METHOD_NONE;
}

int radio_io_init(const ptt_config_t *config)
{
    if (!config)
        return -1;

    pthread_mutex_lock(&g_radio_mutex);

    if (g_backend)
        radio_io_shutdown_locked();

    g_config = *config;
    g_config.device[sizeof(g_config.device) - 1] = '\0';
    if (g_config.hamlib_log_level < 0 || g_config.hamlib_log_level > 6)
    {
        HLOGW(RADIO_LOG_TAG,
              "Invalid hamlib_log_level=%d; using 0 (valid range 0..6)",
              g_config.hamlib_log_level);
        g_config.hamlib_log_level = 0;
    }
    if (g_config.hamlib_serial_speed < 0)
        g_config.hamlib_serial_speed = 0;

    HLOGI(RADIO_LOG_TAG, "Initializing PTT (method=%s, device=%s)",
          ptt_method_name(g_config.method),
          g_config.device[0] ? g_config.device : "none");

    if (g_config.method == PTT_METHOD_NONE)
    {
        HLOGI(RADIO_LOG_TAG, "Direct PTT control disabled");
        pthread_mutex_unlock(&g_radio_mutex);
        return 0;
    }

    const ptt_backend_t *backend = backend_for_method(g_config.method);
    if (!backend)
    {
        HLOGE(RADIO_LOG_TAG, "Unknown PTT method %d", (int)g_config.method);
        g_config.method = PTT_METHOD_NONE;
        pthread_mutex_unlock(&g_radio_mutex);
        return -1;
    }

    if (backend->open(&g_config) != 0)
    {
        backend->close();
        g_config.method = PTT_METHOD_NONE;
        pthread_mutex_unlock(&g_radio_mutex);
        return -1;
    }

    g_backend = backend;
    pthread_mutex_unlock(&g_radio_mutex);
    return 0;
}

void radio_io_shutdown(void)
{
    pthread_mutex_lock(&g_radio_mutex);
    HLOGI(RADIO_LOG_TAG, "Shutting down PTT method %s",
          ptt_method_name(g_config.method));
    radio_io_shutdown_locked();
    pthread_mutex_unlock(&g_radio_mutex);
}

bool radio_io_enabled(void)
{
    pthread_mutex_lock(&g_radio_mutex);
    bool enabled = (g_backend != NULL);
    pthread_mutex_unlock(&g_radio_mutex);
    return enabled;
}

static int radio_io_key(bool on)
{
    pthread_mutex_lock(&g_radio_mutex);
    if (!g_backend)
    {
        HLOGD(RADIO_LOG_TAG, "PTT %s requested while direct PTT is disabled",
              on ? "ON" : "OFF");
        pthread_mutex_unlock(&g_radio_mutex);
        return -1;
    }
    int rc = g_backend->set(on);
    pthread_mutex_unlock(&g_radio_mutex);
    return rc;
}

int radio_io_key_on(void)  { return radio_io_key(true); }
int radio_io_key_off(void) { return radio_io_key(false); }

void radio_io_list_models(void)
{
#ifdef HAVE_HAMLIB
    list_models();
#else
    fprintf(stderr,
            "HAMLIB support not compiled in. Install libhamlib-dev and rebuild.\n");
#endif
}

int radio_io_get_radio_list(char ids[][16], char names[][64], int max_count)
{
#ifdef HAVE_HAMLIB
    return get_radio_list(ids, names, max_count);
#else
    (void)ids;
    (void)names;
    (void)max_count;
    return 0;
#endif
}

int radio_io_restart(const ptt_config_t *config)
{
    if (!config)
        return -1;
    HLOGI(RADIO_LOG_TAG, "PTT restart requested (method=%s, device=%s)",
          ptt_method_name(config->method),
          config->device[0] ? config->device : "none");

    /* radio_io_init() closes the previous backend and opens the requested one
     * while holding g_radio_mutex, so keying cannot slip between the two.
     * Some opens (notably Hamlib rig_open()) can take seconds, so callers must
     * not reconfigure PTT during a live ARQ session: modem keying deliberately
     * blocks for the entire restart to avoid touching a half-open backend. */
    return radio_io_init(config);
}

void radio_io_get_config(ptt_config_t *config)
{
    if (!config)
        return;
    pthread_mutex_lock(&g_radio_mutex);
    *config = g_config;
    pthread_mutex_unlock(&g_radio_mutex);
}

ptt_method_t radio_io_get_ptt_method(void)
{
    pthread_mutex_lock(&g_radio_mutex);
    ptt_method_t method = g_config.method;
    pthread_mutex_unlock(&g_radio_mutex);
    return method;
}

const char *radio_io_get_device_path(void)
{
    static _Thread_local char device[PTT_DEVICE_PATH_MAX];
    pthread_mutex_lock(&g_radio_mutex);
    snprintf(device, sizeof(device), "%s", g_config.device);
    pthread_mutex_unlock(&g_radio_mutex);
    return device;
}

int radio_io_get_radio_type(void)
{
    pthread_mutex_lock(&g_radio_mutex);
    int type = RADIO_TYPE_NONE;
    if (g_config.method == PTT_METHOD_HAMLIB)
        type = g_config.hamlib_model;
    else if (g_config.method == PTT_METHOD_HERMES_SHM)
        type = RADIO_TYPE_SHM;
    pthread_mutex_unlock(&g_radio_mutex);
    return type;
}

int radio_io_get_hamlib_log_level(void)
{
    pthread_mutex_lock(&g_radio_mutex);
    int level = g_config.hamlib_log_level;
    pthread_mutex_unlock(&g_radio_mutex);
    return level;
}

int radio_io_get_serial_speed(void)
{
    pthread_mutex_lock(&g_radio_mutex);
    int speed = g_config.hamlib_serial_speed;
    pthread_mutex_unlock(&g_radio_mutex);
    return speed;
}
