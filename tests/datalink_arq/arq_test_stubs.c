/*
 * Mock stubs for ARQ unit tests
 *
 * Provides stub implementations of external dependencies used by
 * ARQ modules, so tests can link without pulling in modem/audio/etc.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "hermes_log.h"
#include "framer.h"
#include "arq.h"

/* ---- hermes_log stubs ---- */

static uint64_t mock_uptime_value = 0;

uint64_t hermes_uptime_ms(void)
{
    return mock_uptime_value;
}

void mock_set_uptime_ms(uint64_t ms)
{
    mock_uptime_value = ms;
}

void hermes_logf(hermes_log_level_t level, const char *component,
                 const char *fmt, ...)
{
    (void)level; (void)component; (void)fmt;
}

/* ---- framer stubs ---- */

void write_frame_header(uint8_t *data, int packet_type, uint8_t extension)
{
    data[0] = (uint8_t)((packet_type << PACKET_TYPE_SHIFT) | (extension & FRAME_EXT_MASK));
}

int8_t parse_frame_header(const uint8_t *data_frame, uint32_t frame_size, uint8_t *extension_out)
{
    if (!data_frame || frame_size < 1) return -1;
    uint8_t ptype = frame_header_packet_type(data_frame[0]);
    if (extension_out)
        *extension_out = frame_header_extension(data_frame[0]);
    return (int8_t)ptype;
}

/* ---- arq_info global (used by arq.h) ---- */

arq_info arq_conn = {0};

/* ---- arq.c function stubs ---- */

int arq_reported_bandwidth_hz(void)
{
    return ARQ_BANDWIDTH_FULL_HZ; /* default to 2300 Hz for tests */
}

bool arq_bandwidth_allows_mode(int mode)
{
    (void)mode;
    return true; /* allow all modes in tests */
}

/* ---- freedv_gen_crc16 stub (CRC16-CCITT) ---- */

unsigned short freedv_gen_crc16(unsigned char *data_p, int length)
{
    unsigned short crc = 0xFFFF;
    for (int i = 0; i < length; i++) {
        unsigned char x = (unsigned char)(crc >> 8) ^ data_p[i];
        x ^= x >> 4;
        crc = (unsigned short)((crc << 8) ^ ((unsigned short)(x << 12)) ^
              ((unsigned short)(x << 5)) ^ ((unsigned short)x));
    }
    return crc;
}
