/* Mercury Modem — ARQ message-bus channel interface
 *
 * Copyright (C) 2025-2026 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ARQ_CHANNELS_H_
#define ARQ_CHANNELS_H_

#include <stddef.h>

#include "chan.h"
#include "arq_events.h"

#define ARQ_CH_CAP_TCP_CMD 64
#define ARQ_CH_CAP_TCP_PAYLOAD 128
#define ARQ_CH_CAP_MODEM_FRAME 128
#define ARQ_CH_CAP_MODEM_METRICS 128
#define ARQ_CH_CAP_MODEM_TX 128
#define ARQ_CH_CAP_TCP_STATUS 128
#define ARQ_CH_CAP_SHUTDOWN 1

/** @brief ARQ message-bus channels connecting TCP, modem, and ARQ workers. */
typedef struct
{
    chan_t *tcp_cmd;
    chan_t *tcp_payload;
    chan_t *modem_frame;
    chan_t *modem_metrics;
    chan_t *modem_tx;
    chan_t *tcp_status;
    chan_t *shutdown;
} arq_channel_bus_t;

/**
 * @brief Initialize ARQ channel bus.
 * @param bus Bus structure to initialize.
 * @return 0 on success, negative on failure.
 */
int arq_channel_bus_init(arq_channel_bus_t *bus);

/**
 * @brief Close all ARQ channel endpoints to stop producers/consumers.
 * @param bus Bus to close.
 */
void arq_channel_bus_close(arq_channel_bus_t *bus);

/**
 * @brief Dispose ARQ channel resources.
 * @param bus Bus to dispose.
 */
void arq_channel_bus_dispose(arq_channel_bus_t *bus);

/**
 * @brief Try to enqueue a TCP control command into ARQ bus.
 * @param bus Bus instance.
 * @param msg Command message.
 * @return 0 on success, negative if full/error.
 */
int arq_channel_bus_try_send_cmd(arq_channel_bus_t *bus, const arq_cmd_msg_t *msg);

/**
 * @brief Try to enqueue TCP payload bytes into ARQ bus.
 * @param bus Bus instance.
 * @param data Payload bytes.
 * @param len Payload length in bytes.
 * @return 0 on success, negative if full/error.
 */
int arq_channel_bus_try_send_payload(arq_channel_bus_t *bus, const uint8_t *data, size_t len);

/**
 * @brief Receive next TCP command from ARQ bus.
 * @param bus Bus instance.
 * @param msg Output command message.
 * @return 0 on success, negative on closed/error.
 */
int arq_channel_bus_recv_cmd(arq_channel_bus_t *bus, arq_cmd_msg_t *msg);

/**
 * @brief Receive next TCP payload message from ARQ bus.
 * @param bus Bus instance.
 * @param msg Output payload message.
 * @return 0 on success, negative on closed/error.
 */
int arq_channel_bus_recv_payload(arq_channel_bus_t *bus, arq_bytes_msg_t *msg);

#endif /* ARQ_CHANNELS_H_ */
