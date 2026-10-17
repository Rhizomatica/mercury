/* Mercury Modem — ARQ message-bus channels
 *
 * Copyright (C) 2025-2026 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "arq_channels.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "hermes_log.h"

static int channel_open(chan_t **slot, size_t capacity)
{
    chan_t *channel;

    if (!slot)
    {
        errno = EINVAL;
        return -1;
    }

    channel = chan_init(capacity);
    if (!channel)
        return -1;

    *slot = channel;
    return 0;
}

static void channel_close(chan_t *channel)
{
    if (channel)
        (void)chan_close(channel);
}

static void channel_dispose(chan_t **slot)
{
    if (!slot || !*slot)
        return;

    (void)chan_close(*slot);
    chan_dispose(*slot);
    *slot = NULL;
}

int arq_channel_bus_init(arq_channel_bus_t *bus)
{
    if (!bus)
    {
        errno = EINVAL;
        HLOGE("arq-bus", "Init failed: null bus");
        return -1;
    }

    memset(bus, 0, sizeof(*bus));

    if (channel_open(&bus->tcp_cmd, ARQ_CH_CAP_TCP_CMD) < 0 ||
        channel_open(&bus->tcp_payload, ARQ_CH_CAP_TCP_PAYLOAD) < 0 ||
        channel_open(&bus->modem_frame, ARQ_CH_CAP_MODEM_FRAME) < 0 ||
        channel_open(&bus->modem_metrics, ARQ_CH_CAP_MODEM_METRICS) < 0 ||
        channel_open(&bus->modem_tx, ARQ_CH_CAP_MODEM_TX) < 0 ||
        channel_open(&bus->tcp_status, ARQ_CH_CAP_TCP_STATUS) < 0 ||
        channel_open(&bus->shutdown, ARQ_CH_CAP_SHUTDOWN) < 0)
    {
        arq_channel_bus_dispose(bus);
        HLOGE("arq-bus", "Init failed: channel allocation error");
        return -1;
    }

    HLOGI("arq-bus", "Initialized channel bus");
    return 0;
}

void arq_channel_bus_close(arq_channel_bus_t *bus)
{
    if (!bus)
        return;

    channel_close(bus->tcp_cmd);
    channel_close(bus->tcp_payload);
    channel_close(bus->modem_frame);
    channel_close(bus->modem_metrics);
    channel_close(bus->modem_tx);
    channel_close(bus->tcp_status);
    channel_close(bus->shutdown);
    HLOGD("arq-bus", "Closed channel bus");
}

void arq_channel_bus_dispose(arq_channel_bus_t *bus)
{
    if (!bus)
        return;

    channel_dispose(&bus->tcp_cmd);
    channel_dispose(&bus->tcp_payload);
    channel_dispose(&bus->modem_frame);
    channel_dispose(&bus->modem_metrics);
    channel_dispose(&bus->modem_tx);
    channel_dispose(&bus->tcp_status);
    channel_dispose(&bus->shutdown);
    HLOGD("arq-bus", "Disposed channel bus");
}

int arq_channel_bus_try_send_cmd(arq_channel_bus_t *bus, const arq_cmd_msg_t *msg)
{
    chan_t *send_chans[1];
    void *send_msgs[1];
    arq_cmd_msg_t *copy;
    int rc;

    if (!bus || !bus->tcp_cmd || !msg)
    {
        errno = EINVAL;
        return -1;
    }

    copy = (arq_cmd_msg_t *)malloc(sizeof(*copy));
    if (!copy)
        return -1;

    *copy = *msg;
    send_chans[0] = bus->tcp_cmd;
    send_msgs[0] = copy;
    rc = chan_select(NULL, 0, NULL, send_chans, 1, send_msgs);
    if (rc != 0)
    {
        free(copy);
        errno = EAGAIN;
        return -1;
    }

    return 0;
}

int arq_channel_bus_try_send_payload(arq_channel_bus_t *bus, const uint8_t *data, size_t len)
{
    chan_t *send_chans[1];
    void *send_msgs[1];
    arq_bytes_msg_t *copy;
    int rc;

    if (!bus || !bus->tcp_payload || !data || len == 0 || len > INT_BUFFER_SIZE)
    {
        errno = EINVAL;
        return -1;
    }

    copy = (arq_bytes_msg_t *)calloc(1, sizeof(*copy));
    if (!copy)
        return -1;

    copy->len = len;
    memcpy(copy->data, data, len);
    send_chans[0] = bus->tcp_payload;
    send_msgs[0] = copy;
    rc = chan_select(NULL, 0, NULL, send_chans, 1, send_msgs);
    if (rc != 0)
    {
        free(copy);
        errno = EAGAIN;
        return -1;
    }

    return 0;
}

int arq_channel_bus_recv_cmd(arq_channel_bus_t *bus, arq_cmd_msg_t *msg)
{
    void *raw = NULL;

    if (!bus || !bus->tcp_cmd || !msg)
    {
        errno = EINVAL;
        return -1;
    }

    if (chan_recv(bus->tcp_cmd, &raw) != 0)
        return -1;

    if (!raw)
    {
        errno = EINVAL;
        return -1;
    }

    *msg = *((arq_cmd_msg_t *)raw);
    free(raw);
    return 0;
}

int arq_channel_bus_recv_payload(arq_channel_bus_t *bus, arq_bytes_msg_t *msg)
{
    void *raw = NULL;

    if (!bus || !bus->tcp_payload || !msg)
    {
        errno = EINVAL;
        return -1;
    }

    if (chan_recv(bus->tcp_payload, &raw) != 0)
        return -1;

    if (!raw)
    {
        errno = EINVAL;
        return -1;
    }

    *msg = *((arq_bytes_msg_t *)raw);
    free(raw);
    return 0;
}
