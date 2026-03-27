/* HERMES Modem - Radio I/O abstraction
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
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>

#ifdef HAVE_HAMLIB
#include <hamlib/rig.h>
#include "rigctl_parse.h"
#endif

#include "radio_io.h"
#include "../common/hermes_log.h"

#define RADIO_LOG_TAG "radio-io"

#ifdef HAVE_HERMES_SHM
#include "sbitx_io.h"
#include "radio_cmds.h"
#include "shm_utils.h"
#endif

/* Global mutex — protects radio state (g_radio_type, radio pointer,
 * sbitx_connector) so that key_on / key_off cannot race with a
 * shutdown / restart cycle triggered from the UI thread. */
static pthread_mutex_t g_radio_mutex = PTHREAD_MUTEX_INITIALIZER;

static int g_radio_type = RADIO_TYPE_NONE;
static char g_device_path[256] = {0};
#ifdef HAVE_HAMLIB
static RIG *radio = NULL;
#endif

#ifdef HAVE_HERMES_SHM
static controller_conn *sbitx_connector = NULL;
#endif

int radio_io_init(int radio_type, const char *device_path)
{
    pthread_mutex_lock(&g_radio_mutex);
    HLOGI(RADIO_LOG_TAG, "Initializing radio (type=%d, device=%s)",
          radio_type, device_path && device_path[0] ? device_path : "(none)");

    if (radio_type == RADIO_TYPE_NONE)
    {
        g_radio_type = RADIO_TYPE_NONE;
        g_device_path[0] = '\0';
        HLOGI(RADIO_LOG_TAG, "Radio control disabled (type=NONE)");
        pthread_mutex_unlock(&g_radio_mutex);
        return 0;
    }

    g_radio_type = radio_type;
    if (device_path && device_path[0])
        snprintf(g_device_path, sizeof(g_device_path), "%s", device_path);
    else
        g_device_path[0] = '\0';

    if (radio_type == RADIO_TYPE_SHM)
    {
#ifdef HAVE_HERMES_SHM
        if (!shm_is_created(SYSV_SHM_CONTROLLER_KEY_STR, sizeof(controller_conn)))
        {
            HLOGE(RADIO_LOG_TAG, "Radio SHM not created. Is sbitx_controller running?");
            g_radio_type = RADIO_TYPE_NONE;
            pthread_mutex_unlock(&g_radio_mutex);
            return -1;
        }

        sbitx_connector = (controller_conn *) shm_attach(SYSV_SHM_CONTROLLER_KEY_STR,
                                                          sizeof(controller_conn));
        if (!sbitx_connector)
        {
            HLOGE(RADIO_LOG_TAG, "Failed to attach to radio SHM");
            g_radio_type = RADIO_TYPE_NONE;
            pthread_mutex_unlock(&g_radio_mutex);
            return -1;
        }

        HLOGI(RADIO_LOG_TAG, "Radio control: HERMES shared memory interface");
        pthread_mutex_unlock(&g_radio_mutex);
        return 0;
#else
        HLOGE(RADIO_LOG_TAG, "HERMES SHM radio control is only available on Linux builds");
        g_radio_type = RADIO_TYPE_NONE;
        pthread_mutex_unlock(&g_radio_mutex);
        return -1;
#endif
    }

#ifdef HAVE_HAMLIB
    /* Hamlib path */
    HLOGD(RADIO_LOG_TAG, "Calling rig_init(model=%d)", radio_type);
    radio = rig_init(radio_type);
    if (!radio)
    {
        HLOGE(RADIO_LOG_TAG, "Unknown rig num %d, or initialization error.", radio_type);
        HLOGE(RADIO_LOG_TAG, "Please check available radios with -K option.");
        g_radio_type = RADIO_TYPE_NONE;
        pthread_mutex_unlock(&g_radio_mutex);
        return -1;
    }

    if (device_path && device_path[0])
        snprintf(radio->state.rigport.pathname, HAMLIB_FILPATHLEN, "%s", device_path);

    HLOGD(RADIO_LOG_TAG, "Calling rig_open(device=%s)",
          device_path && device_path[0] ? device_path : "(default)");
    int ret = rig_open(radio);
    if (ret != RIG_OK)
    {
        HLOGE(RADIO_LOG_TAG, "rig_open: error = %s %s",
              device_path ? device_path : "(default)", rigerror(ret));
        rig_cleanup(radio);
        radio = NULL;
        g_radio_type = RADIO_TYPE_NONE;
        pthread_mutex_unlock(&g_radio_mutex);
        return -1;
    }

    if (radio->caps->rig_model == RIG_MODEL_NETRIGCTL)
    {
        int rigctld_vfo_opt = netrigctl_get_vfo_mode(radio);
        radio->state.vfo_opt = rigctld_vfo_opt;
    }

    HLOGI(RADIO_LOG_TAG, "Radio control: HAMLIB (model %d, device %s)",
          radio_type, device_path && device_path[0] ? device_path : "(default)");
    pthread_mutex_unlock(&g_radio_mutex);
    return 0;
#else
    HLOGE(RADIO_LOG_TAG, "HAMLIB support not compiled in. Install libhamlib-dev and rebuild.");
    g_radio_type = RADIO_TYPE_NONE;
    pthread_mutex_unlock(&g_radio_mutex);
    return -1;
#endif
}

void radio_io_shutdown(void)
{
    pthread_mutex_lock(&g_radio_mutex);
    HLOGI(RADIO_LOG_TAG, "Shutting down radio (type=%d)", g_radio_type);

#ifdef HAVE_HERMES_SHM
    if (g_radio_type == RADIO_TYPE_SHM)
    {
        if (sbitx_connector)
        {
            shm_dettach(SYSV_SHM_CONTROLLER_KEY_STR,
                        sizeof(controller_conn), sbitx_connector);
            sbitx_connector = NULL;
        }

        g_radio_type = RADIO_TYPE_NONE;
        HLOGI(RADIO_LOG_TAG, "Radio shutdown complete (SHM)");
        pthread_mutex_unlock(&g_radio_mutex);
        return;
    }
#endif

#ifdef HAVE_HAMLIB
    if (g_radio_type > 0 && radio)
    {
        HLOGD(RADIO_LOG_TAG, "Sending PTT OFF before closing rig");
        rig_set_ptt(radio, RIG_VFO_CURR, RIG_PTT_OFF);
        rig_close(radio);
        rig_cleanup(radio);
        radio = NULL;
    }
#endif

    g_radio_type = RADIO_TYPE_NONE;
    HLOGI(RADIO_LOG_TAG, "Radio shutdown complete");
    pthread_mutex_unlock(&g_radio_mutex);
}

bool radio_io_enabled(void)
{
    return g_radio_type != RADIO_TYPE_NONE;
}

void radio_io_key_on(void)
{
    pthread_mutex_lock(&g_radio_mutex);

    if (g_radio_type == RADIO_TYPE_NONE)
    {
        HLOGD(RADIO_LOG_TAG, "key_on called but radio is disabled, ignoring");
        pthread_mutex_unlock(&g_radio_mutex);
        return;
    }

#ifdef HAVE_HERMES_SHM
    if (g_radio_type == RADIO_TYPE_SHM)
    {
        uint8_t srv_cmd[5];
        uint8_t response[5];

        memset(srv_cmd, 0, 5);
        memset(response, 0, 5);

        srv_cmd[4] = CMD_PTT_ON;
        radio_cmd(sbitx_connector, srv_cmd, response);

        HLOGD(RADIO_LOG_TAG, "PTT ON via SHM");
        pthread_mutex_unlock(&g_radio_mutex);
        return;
    }
#endif

#ifdef HAVE_HAMLIB
    if (g_radio_type > 0 && radio)
    {
        rig_set_ptt(radio, RIG_VFO_CURR, RIG_PTT_ON);
        HLOGD(RADIO_LOG_TAG, "PTT ON via HAMLIB (model %d)", g_radio_type);
    }
#endif

    pthread_mutex_unlock(&g_radio_mutex);
}

void radio_io_key_off(void)
{
    pthread_mutex_lock(&g_radio_mutex);

    if (g_radio_type == RADIO_TYPE_NONE)
    {
        HLOGD(RADIO_LOG_TAG, "key_off called but radio is disabled, ignoring");
        pthread_mutex_unlock(&g_radio_mutex);
        return;
    }

#ifdef HAVE_HERMES_SHM
    if (g_radio_type == RADIO_TYPE_SHM)
    {
        uint8_t srv_cmd[5];
        uint8_t response[5];

        memset(srv_cmd, 0, 5);
        memset(response, 0, 5);

        srv_cmd[4] = CMD_PTT_OFF;
        radio_cmd(sbitx_connector, srv_cmd, response);

        HLOGD(RADIO_LOG_TAG, "PTT OFF via SHM");
        pthread_mutex_unlock(&g_radio_mutex);
        return;
    }
#endif

#ifdef HAVE_HAMLIB
    if (g_radio_type > 0 && radio)
    {
        rig_set_ptt(radio, RIG_VFO_CURR, RIG_PTT_OFF);
        HLOGD(RADIO_LOG_TAG, "PTT OFF via HAMLIB (model %d)", g_radio_type);
    }
#endif

    pthread_mutex_unlock(&g_radio_mutex);
}

void radio_io_list_models(void)
{
#ifdef HAVE_HAMLIB
    list_models();
#else
    fprintf(stderr, "HAMLIB support not compiled in. Install libhamlib-dev and rebuild.\n");
#endif
}

int radio_io_get_radio_list(char ids[][16], char names[][64], int max_count)
{
#ifdef HAVE_HAMLIB
    return get_radio_list(ids, names, max_count);
#else
    (void)ids; (void)names; (void)max_count;
    return 0;
#endif
}

int radio_io_restart(int new_radio_type, const char *device_path)
{
    HLOGI(RADIO_LOG_TAG, "Restart requested (new_type=%d, device=%s)",
          new_radio_type, device_path && device_path[0] ? device_path : "(none)");

    /* radio_io_shutdown / radio_io_init both acquire the mutex internally
     * which serialises this restart against any concurrent key_on/key_off. */
    radio_io_shutdown();
    return radio_io_init(new_radio_type, device_path);
}

const char *radio_io_get_device_path(void)
{
    return g_device_path;
}

int radio_io_get_radio_type(void)
{
    return g_radio_type;
}
