/* libFuzzer target: ARQ frame-header decoder.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Feeds arbitrary bytes to arq_protocol_decode_hdr(), which parses the 8-byte
 * on-air ARQ header. The decoder must never read past buf_len nor write past
 * the fixed-size output struct, for any input length (including 0).
 */
#include <stddef.h>
#include <stdint.h>

#include "arq_protocol.h"

/* arq_protocol.c references freedv_gen_crc16() from arq_protocol_callsign_crc16(),
 * a function our harness never calls. Provide a stub so the harness links without
 * the full FreeDV library. The stub is never reached during fuzzing. */
uint16_t freedv_gen_crc16(unsigned char *data_p, int length)
{
    (void)data_p; (void)length;
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    arq_frame_hdr_t hdr;
    (void)arq_protocol_decode_hdr(data, size, &hdr);
    return 0;
}
