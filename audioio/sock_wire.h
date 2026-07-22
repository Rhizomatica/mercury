/* Wire codec for the -x sock lockstep bench transport.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * One frame per audio block over AF_UNIX SOCK_STREAM; all integers are
 * little-endian regardless of host order.  The contract is shared with the
 * skywave bench (sock_frames.py) and pinned byte-exact by
 * tests/audioio/test_sock_wire.c:
 *
 *   u32 len                          bytes after this field
 *   sim -> station:  u64 seq | u64 virtual_now_ms | u16 n | n i16 samples
 *   station -> sim:  u64 seq | u8 ptt | u16 n | n i16 samples
 *
 * Samples are mono 8 kHz i16 on the wire; the modem rings hold i32
 * (i16 << 16), the same convention as the FIFO path.
 */
#ifndef SOCK_WIRE_H_
#define SOCK_WIRE_H_

#include <stddef.h>
#include <stdint.h>

#define SOCK_WIRE_HDR_SIM_BYTES 18   /* u64 seq + u64 virtual_now_ms + u16 n */
#define SOCK_WIRE_HDR_STA_BYTES 11   /* u64 seq + u8 ptt + u16 n             */
#define SOCK_WIRE_MAX_SAMPLES   65535u   /* n is a u16                       */

#define SOCK_WIRE_PTT_OFF     0
#define SOCK_WIRE_PTT_ON      1
#define SOCK_WIRE_PTT_UNKNOWN 255    /* station cannot see the modem's PTT   */

static inline uint16_t sock_wire_rd_u16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static inline uint32_t sock_wire_rd_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline uint64_t sock_wire_rd_u64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--)
        v = (v << 8) | p[i];
    return v;
}

static inline void sock_wire_wr_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)(v >> 8);
}

static inline void sock_wire_wr_u32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++)
    {
        p[i] = (uint8_t)(v & 0xff);
        v >>= 8;
    }
}

static inline void sock_wire_wr_u64(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++)
    {
        p[i] = (uint8_t)(v & 0xff);
        v >>= 8;
    }
}

/* i16 wire sample <-> i32 modem-ring sample */
static inline int32_t sock_wire_i16_to_ring(uint16_t raw)
{
    /* Shift in unsigned then convert back: a signed left-shift of a negative
     * value (raw == 0x8000 -> -32768) is undefined behaviour (UBSan abort);
     * the unsigned shift is well-defined and byte-identical on 2's-complement. */
    return (int32_t)((uint32_t)(int16_t) raw << 16);
}

static inline int16_t sock_wire_ring_to_i16(int32_t s)
{
    return (int16_t)(s >> 16);
}

/* Parse a sim frame header (the SOCK_WIRE_HDR_SIM_BYTES that follow the u32
 * length prefix), cross-checking the prefix against the sample count.
 * Returns 0 on success, -1 if len is inconsistent with the header. */
static inline int sock_wire_parse_sim(const uint8_t *hdr, uint32_t len,
                                      uint64_t *seq, uint64_t *vnow_ms,
                                      uint16_t *n)
{
    uint16_t count = sock_wire_rd_u16(hdr + 16);
    if ((uint32_t)(SOCK_WIRE_HDR_SIM_BYTES + count * sizeof(int16_t)) != len)
        return -1;
    *seq     = sock_wire_rd_u64(hdr);
    *vnow_ms = sock_wire_rd_u64(hdr + 8);
    *n       = count;
    return 0;
}

/* Serialize a complete station frame (length prefix + header + n samples)
 * into dst, which must hold 4 + SOCK_WIRE_HDR_STA_BYTES + 2n bytes.
 * ring[0..have) supplies samples; the remainder is silence-padded.
 * Returns the total frame size in bytes. */
static inline size_t sock_wire_build_station(uint8_t *dst, uint64_t seq,
                                             uint8_t ptt, uint16_t n,
                                             const int32_t *ring, size_t have)
{
    sock_wire_wr_u32(dst, (uint32_t)(SOCK_WIRE_HDR_STA_BYTES + n * sizeof(int16_t)));
    sock_wire_wr_u64(dst + 4, seq);
    dst[12] = ptt;
    sock_wire_wr_u16(dst + 13, n);
    uint8_t *p = dst + 4 + SOCK_WIRE_HDR_STA_BYTES;
    for (uint16_t i = 0; i < n; i++)
    {
        int16_t s = (i < have) ? sock_wire_ring_to_i16(ring[i]) : 0;
        sock_wire_wr_u16(p + (size_t)i * 2, (uint16_t) s);
    }
    return 4 + SOCK_WIRE_HDR_STA_BYTES + (size_t)n * sizeof(int16_t);
}

#endif /* SOCK_WIRE_H_ */
