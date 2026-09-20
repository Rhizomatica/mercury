/* RFC 6455 frame codec
 *
 * Copyright (C) 2026 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "ws_frame.h"

bool ws_frame_is_control(int opcode)
{
    return (opcode & 0x08) != 0;
}

int ws_frame_parse(const uint8_t *buf, size_t avail, bool require_mask,
                   ws_frame_t *f)
{
    uint64_t plen;
    size_t hlen;
    int i;

    if (f == NULL || buf == NULL)
        return WS_FRAME_PROTO_ERROR;

    if (avail < 2)
        return WS_FRAME_INCOMPLETE;

    f->fin = (buf[0] & 0x80) != 0;
    f->opcode = buf[0] & 0x0F;
    f->masked = (buf[1] & 0x80) != 0;
    f->mask = NULL;

    /* We negotiate no extensions, so a reserved bit is a protocol error
     * rather than something to skip past. */
    if ((buf[0] & 0x70) != 0)
        return WS_FRAME_PROTO_ERROR;

    plen = (uint64_t) (buf[1] & 0x7F);
    hlen = 2;

    if (plen == 126)
    {
        if (avail < 4)
            return WS_FRAME_INCOMPLETE;
        plen = ((uint64_t) buf[2] << 8) | buf[3];
        hlen = 4;

        /* A 16-bit length must actually need 16 bits; the minimal encoding is
         * mandatory, and accepting a padded one lets two implementations
         * disagree about the same bytes. */
        if (plen < 126)
            return WS_FRAME_PROTO_ERROR;
    }
    else if (plen == 127)
    {
        if (avail < 10)
            return WS_FRAME_INCOMPLETE;

        plen = 0;
        for (i = 0; i < 8; i++)
            plen = (plen << 8) | buf[2 + i];
        hlen = 10;

        /* The high bit must be clear (RFC 6455 5.2). */
        if ((plen >> 63) != 0)
            return WS_FRAME_PROTO_ERROR;
        if (plen <= 0xFFFF)
            return WS_FRAME_PROTO_ERROR;
    }

    if (require_mask && !f->masked)
        return WS_FRAME_PROTO_ERROR;

    /* Control frames are never fragmented and never longer than 125 bytes. */
    if (ws_frame_is_control(f->opcode) && (!f->fin || plen > 125))
        return WS_FRAME_PROTO_ERROR;

    /* Opcodes outside the ones RFC 6455 defines are a protocol error. */
    if (f->opcode > WS_OP_BINARY && f->opcode < WS_OP_CLOSE)
        return WS_FRAME_PROTO_ERROR;
    if (f->opcode > WS_OP_PONG)
        return WS_FRAME_PROTO_ERROR;

    if (f->masked)
    {
        if (avail < hlen + 4)
            return WS_FRAME_INCOMPLETE;
        f->mask = buf + hlen;
        hlen += 4;
    }

    f->payload_len = plen;
    f->header_len = hlen;
    f->total_len = hlen + (size_t) plen;

    return WS_FRAME_OK;
}

void ws_frame_unmask(uint8_t *payload, size_t len, const uint8_t mask[4])
{
    size_t i;

    if (payload == NULL || mask == NULL)
        return;

    for (i = 0; i < len; i++)
        payload[i] ^= mask[i & 3];
}

size_t ws_frame_header(uint8_t hdr[WS_FRAME_MAX_HEADER], int opcode,
                       size_t payload_len, bool fin)
{
    hdr[0] = (uint8_t) ((fin ? 0x80 : 0x00) | (opcode & 0x0F));

    if (payload_len < 126)
    {
        hdr[1] = (uint8_t) payload_len;
        return 2;
    }

    if (payload_len <= 0xFFFF)
    {
        hdr[1] = 126;
        hdr[2] = (uint8_t) (payload_len >> 8);
        hdr[3] = (uint8_t) payload_len;
        return 4;
    }

    {
        int i;

        hdr[1] = 127;
        for (i = 0; i < 8; i++)
            hdr[2 + i] = (uint8_t) ((uint64_t) payload_len >> (56 - i * 8));
        return 10;
    }
}
