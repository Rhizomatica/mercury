/* Mercury radio interface (radio_io/radio_io.h), daemon build
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * PTT and the dial frequency come from the host program, through the ops it
 * registers with mercury_host_set_radio().  There is no radio for Mercury to
 * configure here, so the UI's radio list is empty and a PTT reconfiguration
 * from the UI is refused.
 */

#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "hermes_log.h"
#include "mercury_host.h"
#include "radio_io.h"

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static mercury_host_radio_ops s_ops;
static bool s_keyed = false;

void mercury_host_set_radio(const mercury_host_radio_ops *ops)
{
    pthread_mutex_lock(&s_lock);
    if (ops)
        s_ops = *ops;
    else
        memset(&s_ops, 0, sizeof(s_ops));
    pthread_mutex_unlock(&s_lock);
}

int radio_io_init(const ptt_config_t *config)
{
    (void) config;
    if (!radio_io_enabled())
        HLOGW("radio-host", "The host program registered no PTT: Mercury cannot key");
    return 0;
}

bool radio_io_enabled(void)
{
    pthread_mutex_lock(&s_lock);
    bool on = s_ops.key != NULL;
    pthread_mutex_unlock(&s_lock);
    return on;
}

/* Under s_lock, so the host never sees an unkey overtake the key before it
 * from another thread. */
static int radio_host_key(bool tx)
{
    int rc = -1;
    pthread_mutex_lock(&s_lock);
    if (s_ops.key)
    {
        rc = s_ops.key(tx, s_ops.user);
        if (rc == 0)
            s_keyed = tx;
    }
    pthread_mutex_unlock(&s_lock);
    if (rc != 0)
        HLOGW("radio-host", "PTT %s refused by the host (%d)", tx ? "ON" : "OFF", rc);
    return rc;
}

int radio_io_key_on(void)  { return radio_host_key(true); }
int radio_io_key_off(void) { return radio_host_key(false); }

void radio_io_shutdown(void)
{
    pthread_mutex_lock(&s_lock);
    bool keyed = s_keyed;
    pthread_mutex_unlock(&s_lock);
    if (keyed)
        radio_host_key(false);
}

bool radio_io_get_frequency(bool allow_poll, uint64_t *frequency_hz, uint64_t *age_ms)
{
    (void) allow_poll;
    if (!frequency_hz || !age_ms)
        return false;

    /* trylock: a key() blocking on the radio must not hold up the UI. */
    if (pthread_mutex_trylock(&s_lock) != 0)
        return false;
    bool ok = s_ops.get_frequency && s_ops.get_frequency(frequency_hz, s_ops.user);
    pthread_mutex_unlock(&s_lock);
    if (ok)
        *age_ms = 0;
    return ok;
}

void radio_io_report_frequency(uint64_t frequency_hz)
{
    (void) frequency_hz;            /* the host owns the radio's frequency */
}

void radio_io_set_usb_fd(const char *device, int fd)
{
    (void) device; (void) fd;       /* the Android app's; the bridge calls it */
}

/* ---- the rest of radio_io.h: nothing for Mercury to configure ---- */

void radio_io_list_models(void)
{
    printf("PTT comes from the host program; there are no radio models to list.\n");
}

int radio_io_get_radio_list(char ids[][16], char names[][64], int max_count)
{
    (void) ids; (void) names; (void) max_count;
    return 0;
}

int radio_io_restart(const ptt_config_t *config)
{
    (void) config;
    HLOGW("radio-host", "PTT belongs to the host program; not reconfigured");
    return -1;
}

void radio_io_get_config(ptt_config_t *config)
{
    if (config)
    {
        memset(config, 0, sizeof(*config));
        config->method = PTT_METHOD_NONE;
    }
}

ptt_method_t radio_io_get_ptt_method(void)
{
    return PTT_METHOD_NONE;
}

const char *radio_io_get_device_path(void)
{
    return "";
}

int radio_io_get_hamlib_log_level(void)
{
    return 0;
}
