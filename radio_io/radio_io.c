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

#ifdef HAVE_HAMLIB
#include <hamlib/rig.h>
#include "rigctl_parse.h"
#endif

#include "radio_io.h"

#ifdef HAVE_HERMES_SHM
#include "sbitx_io.h"
#include "radio_cmds.h"
#include "shm_utils.h"
#endif

static int g_radio_type = RADIO_TYPE_NONE;
#ifdef HAVE_HAMLIB
static RIG *radio = NULL;
#endif

#ifdef HAVE_HERMES_SHM
static controller_conn *sbitx_connector = NULL;
#endif

int radio_io_init(int radio_type, const char *device_path)
{
    if (radio_type == RADIO_TYPE_NONE)
        return 0;

    g_radio_type = radio_type;

    if (radio_type == RADIO_TYPE_SHM)
    {
#ifdef HAVE_HERMES_SHM
        if (!shm_is_created(SYSV_SHM_CONTROLLER_KEY_STR, sizeof(controller_conn)))
        {
            fprintf(stderr, "Radio SHM not created. Is sbitx_controller running?\n");
            g_radio_type = RADIO_TYPE_NONE;
            return -1;
        }

        sbitx_connector = (controller_conn *) shm_attach(SYSV_SHM_CONTROLLER_KEY_STR,
                                                          sizeof(controller_conn));
        if (!sbitx_connector)
        {
            fprintf(stderr, "Failed to attach to radio SHM.\n");
            g_radio_type = RADIO_TYPE_NONE;
            return -1;
        }

        printf("Radio control: HERMES shared memory interface\n");
        return 0;
#else
        fprintf(stderr, "HERMES shared memory radio control is only available on Linux builds.\n");
        g_radio_type = RADIO_TYPE_NONE;
        return -1;
#endif
    }

#ifdef HAVE_HAMLIB
    /* Hamlib path */
    radio = rig_init(radio_type);
    if (!radio)
    {
        fprintf(stderr, "Unknown rig num %d, or initialization error.\n", radio_type);
        fprintf(stderr, "Please check available radios with -K option.\n");
        g_radio_type = RADIO_TYPE_NONE;
        return -1;
    }

    if (device_path && device_path[0])
        snprintf(radio->state.rigport.pathname, HAMLIB_FILPATHLEN, "%s", device_path);

    int ret = rig_open(radio);
    if (ret != RIG_OK)
    {
        fprintf(stderr, "rig_open: error = %s %s\n",
                device_path ? device_path : "(default)", rigerror(ret));
        rig_cleanup(radio);
        radio = NULL;
        g_radio_type = RADIO_TYPE_NONE;
        return -1;
    }

    if (radio->caps->rig_model == RIG_MODEL_NETRIGCTL)
    {
        int rigctld_vfo_opt = netrigctl_get_vfo_mode(radio);
        radio->state.vfo_opt = rigctld_vfo_opt;
    }

    printf("Radio control: HAMLIB (model %d, device %s)\n",
           radio_type, device_path && device_path[0] ? device_path : "(default)");
    return 0;
#else
    fprintf(stderr, "HAMLIB support not compiled in. Install libhamlib-dev and rebuild.\n");
    g_radio_type = RADIO_TYPE_NONE;
    return -1;
#endif
}

void radio_io_shutdown(void)
{
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
        return;
    }
#endif

#ifdef HAVE_HAMLIB
    if (g_radio_type > 0 && radio)
    {
        rig_close(radio);
        rig_cleanup(radio);
        radio = NULL;
    }
#endif

    g_radio_type = RADIO_TYPE_NONE;
}

bool radio_io_enabled(void)
{
    return g_radio_type != RADIO_TYPE_NONE;
}

void radio_io_key_on(void)
{
#ifdef HAVE_HERMES_SHM
    if (g_radio_type == RADIO_TYPE_SHM)
    {
        uint8_t srv_cmd[5];
        uint8_t response[5];

        memset(srv_cmd, 0, 5);
        memset(response, 0, 5);

        srv_cmd[4] = CMD_PTT_ON;
        radio_cmd(sbitx_connector, srv_cmd, response);

        return;
    }
#endif

#ifdef HAVE_HAMLIB
    if (g_radio_type > 0 && radio)
    {
        rig_set_ptt(radio, RIG_VFO_CURR, RIG_PTT_ON);
    }
#endif
}

void radio_io_key_off(void)
{
#ifdef HAVE_HERMES_SHM
    if (g_radio_type == RADIO_TYPE_SHM)
    {
        uint8_t srv_cmd[5];
        uint8_t response[5];

        memset(srv_cmd, 0, 5);
        memset(response, 0, 5);

        srv_cmd[4] = CMD_PTT_OFF;
        radio_cmd(sbitx_connector, srv_cmd, response);

        return;
    }
#endif

#ifdef HAVE_HAMLIB
    if (g_radio_type > 0 && radio)
    {
        rig_set_ptt(radio, RIG_VFO_CURR, RIG_PTT_OFF);
    }
#endif
}

void radio_io_list_models(void)
{
#ifdef HAVE_HAMLIB
    list_models();
#else
    fprintf(stderr, "HAMLIB support not compiled in. Install libhamlib-dev and rebuild.\n");
#endif
}
