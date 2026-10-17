/* RFC 6455 frame codec
 *
 * Copyright (C) 2026 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Pure functions: no sockets, no allocation, no state.  Kept apart from
 * ws_server.c so the wire format can be tested directly -- the framing is the
 * part of a WebSocket server that is easy to get subtly wrong (the three
 * length encodings, the mask, the rules that only apply to control frames)
 * and hard to notice when it is wrong against one forgiving client.
 */

#ifndef WS_FRAME_H_
#define WS_FRAME_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WS_OP_CONT   0x0
#define WS_OP_TEXT   0x1
#define WS_OP_BINARY 0x2
#define WS_OP_CLOSE  0x8
#define WS_OP_PING   0x9
#define WS_OP_PONG   0xA

/* Largest header we ever write or read: 2 + 8 length + 4 mask. */
#define WS_FRAME_MAX_HEADER 14

typedef struct {
    bool fin;
    int opcode;
    bool masked;
    uint64_t payload_len;
    const uint8_t *mask;      /* points into the caller's buffer, or NULL */
    size_t header_len;        /* bytes before the payload, mask included */
    size_t total_len;         /* header_len + payload_len */
} ws_frame_t;

#define WS_FRAME_INCOMPLETE   0    /* header not all here yet */
#define WS_FRAME_OK           1
#define WS_FRAME_PROTO_ERROR (-1)  /* peer violated the protocol; close 1002 */

/* Parse a frame header out of `buf`.  Returns WS_FRAME_OK when the header is
 * complete and valid -- the caller must still check `total_len` against how
 * much it holds before touching the payload.  `require_mask` enforces the
 * rule that frames from a client are always masked (RFC 6455 5.1). */
int ws_frame_parse(const uint8_t *buf, size_t avail, bool require_mask,
                   ws_frame_t *f);

/* Unmask a payload in place. */
void ws_frame_unmask(uint8_t *payload, size_t len, const uint8_t mask[4]);

/* Write a server-to-client header (never masked).  Returns its length. */
size_t ws_frame_header(uint8_t hdr[WS_FRAME_MAX_HEADER], int opcode,
                       size_t payload_len, bool fin);

/* True for opcodes 0x8..0xF. */
bool ws_frame_is_control(int opcode);

#endif /* WS_FRAME_H_ */
