/* Mercury host API: chat and the listen mode (host/mercury_host.h)
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * What a host program (hermes-radio-daemon) asks of the running engine
 * without a TCP client: the chat lines as message_store keeps them, sending
 * one, and the broadcast plane's listen mode as the MODE command sets it.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "mercury_host.h"
#include "message_store.h"
#include "arq.h"
#include "modem.h"
#include "mercury_cli.h"
#include "tcp_interfaces.h"
#include "bcast_file.h"
#include "bcast_carousel.h"
#include "broadcast.h"

/* ---- chat ---- */

size_t mercury_host_chat_since(uint64_t after_seq, char *out, size_t out_len, uint64_t *last_seq)
{
    return msg_store_since(after_seq, out, out_len, last_seq);
}

uint64_t mercury_host_chat_last_seq(void)
{
    return msg_store_last_seq();
}

/* The longest chat line: message_store's limit. */
#define HOST_CHAT_MAX 1024

int mercury_host_chat_send(const char *plane, const char *text)
{
    if (!plane || !text)
        return MERCURY_HOST_BAD;
    size_t len = strlen(text);
    while (len > 0 && (text[len - 1] == '\n' || text[len - 1] == '\r'))
        len--;
    if (len == 0 || len > HOST_CHAT_MAX)
        return MERCURY_HOST_BAD;

    if (strcmp(plane, MSG_PLANE_ARQ) == 0)
    {
        /* A line as the chat client writes it: text and "\n".  Stored as
         * sent once the session's queue takes it (arq_submit_tcp_payload). */
        uint8_t line[HOST_CHAT_MAX + 1];
        if (!arq_is_link_connected())
            return MERCURY_HOST_NO_LINK;
        memcpy(line, text, len);
        line[len] = '\n';
        return arq_submit_tcp_payload(line, len + 1) == 0 ? MERCURY_HOST_OK : MERCURY_HOST_FULL;
    }
    if (strcmp(plane, MSG_PLANE_BCAST) == 0)
    {
        int rc = broadcast_send_message((const uint8_t *)text, len);
        return rc == 0 ? MERCURY_HOST_OK : rc == -2 ? MERCURY_HOST_TOO_LONG : MERCURY_HOST_BAD;
    }
    return MERCURY_HOST_BAD;
}

/* ---- listen mode ---- */

int mercury_host_mode_count(void)
{
    return mercury_cli_mode_count();
}

const char *mercury_host_mode_name(int index)
{
    return index >= 0 && index < mercury_cli_mode_count() ? freedv_mode_names[index] : NULL;
}

bool mercury_host_mode_switchable(int index)
{
    return index >= 0 && index < mercury_cli_mode_count() &&
           modem_listen_mode_switchable(freedv_modes[index]);
}

bool mercury_host_mode_carries_files(int index)
{
    return mercury_host_mode_switchable(index) && bcast_file_mode_usable(index);
}

size_t mercury_host_bcast_chat_max(void)
{
    return broadcast_message_max();
}

int mercury_host_listen_mode(void)
{
    int mode = modem_get_listen_mode();
    for (int i = 0; i < mercury_cli_mode_count(); i++)
        if (freedv_modes[i] == mode)
            return i;
    return -1;
}

int mercury_host_set_listen_mode(int index)
{
    if (index < 0 || index >= mercury_cli_mode_count())
        return MERCURY_HOST_BAD;
    int rc = modem_set_listen_mode(freedv_modes[index]);
    return rc == 0 ? MERCURY_HOST_OK : rc == MODEM_LISTEN_MODE_BUSY ? MERCURY_HOST_BUSY : MERCURY_HOST_BAD;
}

/* ---- the broadcast carousel ---- */

int mercury_host_bcast_send(const char *name, const uint8_t *data, size_t len, int cycles,
                            char *err, size_t errlen)
{
    int rc = bcast_carousel_send(name, data, len, cycles, err, errlen);
    return rc == 0 ? MERCURY_HOST_OK : rc == BCAST_CAROUSEL_BUSY ? MERCURY_HOST_BUSY : MERCURY_HOST_BAD;
}

void mercury_host_bcast_stop(void)
{
    bcast_carousel_stop();
}

void mercury_host_bcast_stats(mercury_host_bcast_stats_t *out)
{
    bcast_car_status_t st;
    if (!out)
        return;
    bcast_carousel_get(&st);
    memset(out, 0, sizeof(*out));
    out->state = bcast_car_state_name(st.state);
    out->mode = st.mode;
    out->cycle = st.cycle;
    out->cycles_total = st.cycles_total;
    out->frames_sent = st.frames_sent;
    out->bytes = st.bytes;
    memcpy(out->name, st.name, sizeof(out->name));
}

const char *mercury_host_busy_reason(void)
{
    if (arq_session_active())
        return "arq";
    if (bcast_carousel_sending())
        return "broadcast";
    return NULL;
}
