/* Mercury Modem — ARQ event and command definitions
 *
 * Copyright (C) 2025-2026 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ARQ_EVENTS_H_
#define ARQ_EVENTS_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "defines_modem.h"

#define ARQ_EVENT_TEXT_MAX 16

typedef enum
{
    ARQ_CMD_NONE = 0,
    ARQ_CMD_CLIENT_CONNECT = 1,
    ARQ_CMD_CLIENT_DISCONNECT = 2,
    ARQ_CMD_LISTEN_ON = 3,
    ARQ_CMD_LISTEN_OFF = 4,
    ARQ_CMD_SET_CALLSIGN = 5,
    ARQ_CMD_SET_PUBLIC = 6,
    ARQ_CMD_SET_BANDWIDTH = 7,
    ARQ_CMD_CONNECT = 8,
    ARQ_CMD_DISCONNECT = 9,
    ARQ_CMD_SET_RETRY = 10,
    ARQ_CMD_SEND_CQ = 11
} arq_cmd_type_t;

typedef enum
{
    ARQ_STATUS_NONE = 0,
    ARQ_STATUS_CONNECTED = 1,
    ARQ_STATUS_DISCONNECTED = 2,
    ARQ_STATUS_BUFFER = 3,
    ARQ_STATUS_SN = 4,
    ARQ_STATUS_BITRATE = 5,
    ARQ_STATUS_ERROR = 6
} arq_status_type_t;

typedef enum
{
    ARQ_BUS_MSG_NONE = 0,
    ARQ_BUS_MSG_TCP_CMD = 1,
    ARQ_BUS_MSG_TCP_PAYLOAD = 2,
    ARQ_BUS_MSG_MODEM_FRAME = 3,
    ARQ_BUS_MSG_MODEM_METRICS = 4,
    ARQ_BUS_MSG_MODEM_TX = 5,
    ARQ_BUS_MSG_TCP_STATUS = 6,
    ARQ_BUS_MSG_SHUTDOWN = 7
} arq_bus_msg_type_t;

typedef struct
{
    arq_cmd_type_t type;
    char arg0[ARQ_EVENT_TEXT_MAX];
    char arg1[ARQ_EVENT_TEXT_MAX];
    int32_t value;
    bool flag;
} arq_cmd_msg_t;

typedef struct
{
    size_t len;
    uint8_t data[INT_BUFFER_SIZE];
} arq_bytes_msg_t;

typedef struct
{
    size_t frame_size;
    uint8_t frame[INT_BUFFER_SIZE];
    int packet_type;
    int decoder_mode;
    bool from_control_decoder;
} arq_frame_msg_t;

typedef struct
{
    int mode;
    size_t frame_size;
    uint8_t frame[INT_BUFFER_SIZE];
} arq_tx_frame_msg_t;

typedef struct
{
    int sync;
    float snr;
    int rx_status;
    bool frame_decoded;
} arq_modem_metrics_msg_t;

typedef struct
{
    arq_status_type_t type;
    uint32_t value_u32;
    uint32_t aux_u32;
    float value_f32;
    char text[ARQ_EVENT_TEXT_MAX];
} arq_status_msg_t;

typedef struct
{
    arq_bus_msg_type_t type;
    union
    {
        arq_cmd_msg_t cmd;
        arq_bytes_msg_t bytes;
        arq_frame_msg_t frame;
        arq_tx_frame_msg_t tx;
        arq_modem_metrics_msg_t metrics;
        arq_status_msg_t status;
    } u;
} arq_bus_msg_t;

#endif /* ARQ_EVENTS_H_ */
