/* Wire codec for the -x rtp transport (radio <-> modem over RTP multicast).
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The radio daemon streams its RX signal as ka9q-radio style RTP and the
 * modem answers with a TX stream paced by it (hermes-radio-daemon
 * docs/RTP-AUDIO.md).  Both directions use:
 *
 *   RTP v2, payload type 125 = 8000 Hz mono S16BE (ka9q-radio's PT table),
 *   160 samples (20 ms) per packet, timestamps in samples.
 *
 * TX adds in-stream PTT: the first packet of a transmission carries the
 * marker bit; a packet with an empty payload ends it.
 *
 * Status packets (ka9q-radio TLVs) go to the group's port 5006 from the
 * data socket.  Pinned by tests/audioio/test_rtp_wire.c.
 */
#ifndef RTP_WIRE_H_
#define RTP_WIRE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define RTP_WIRE_PT          125     /* 8000 Hz, 1 channel, S16BE */
#define RTP_WIRE_RATE        8000
#define RTP_WIRE_FRAME       160     /* samples per packet (20 ms) */
#define RTP_WIRE_HDR_BYTES   12
#define RTP_WIRE_MAX_BYTES   (RTP_WIRE_HDR_BYTES + RTP_WIRE_FRAME * 2)
#define RTP_WIRE_DATA_PORT   5004
#define RTP_WIRE_STATUS_PORT 5006

typedef struct {
    bool           marker;
    uint8_t        pt;
    uint16_t       seq;
    uint32_t       ts;
    uint32_t       ssrc;
    const uint8_t *payload;
    size_t         payload_len;
} rtp_wire_hdr;

static inline uint32_t rtp_wire_rd_be(const uint8_t *p, int n)
{
    uint32_t v = 0;
    for (int i = 0; i < n; i++)
        v = (v << 8) | p[i];
    return v;
}

static inline void rtp_wire_wr_be(uint8_t *p, uint32_t v, int n)
{
    for (int i = n - 1; i >= 0; i--, v >>= 8)
        p[i] = (uint8_t)(v & 0xff);
}

/* Parse an RTP packet, skipping CSRCs, a header extension and padding.
 * Returns 0 on success, -1 if it is not a well-formed RTP v2 packet. */
static inline int rtp_wire_parse(const uint8_t *pkt, size_t len, rtp_wire_hdr *h)
{
    if (len < RTP_WIRE_HDR_BYTES || (pkt[0] >> 6) != 2)
        return -1;

    size_t off = RTP_WIRE_HDR_BYTES + 4u * (pkt[0] & 0x0f);
    if (off > len)
        return -1;
    if (pkt[0] & 0x10)                       /* header extension */
    {
        if (off + 4 > len)
            return -1;
        off += 4 + 4u * rtp_wire_rd_be(pkt + off + 2, 2);
        if (off > len)
            return -1;
    }
    size_t end = len;
    if (pkt[0] & 0x20)                       /* padding: last byte is its length */
    {
        uint8_t pad = pkt[len - 1];
        if (pad == 0 || pad > len - off)
            return -1;
        end -= pad;
    }

    h->marker      = (pkt[1] & 0x80) != 0;
    h->pt          = pkt[1] & 0x7f;
    h->seq         = (uint16_t) rtp_wire_rd_be(pkt + 2, 2);
    h->ts          = rtp_wire_rd_be(pkt + 4, 4);
    h->ssrc        = rtp_wire_rd_be(pkt + 8, 4);
    h->payload     = pkt + off;
    h->payload_len = end - off;
    return 0;
}

/* S16BE wire sample <-> i32 modem-ring sample (i16 << 16). */
static inline int32_t rtp_wire_s16be_to_ring(const uint8_t *p)
{
    uint16_t raw = (uint16_t)((p[0] << 8) | p[1]);
    /* unsigned shift: -32768 << 16 would be UB as a signed shift */
    return (int32_t)((uint32_t)(int16_t) raw << 16);
}

static inline void rtp_wire_ring_to_s16be(uint8_t *p, int32_t s)
{
    uint16_t v = (uint16_t)(int16_t)(s >> 16);
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xff);
}

/* Build a PT 125 packet of n samples (0 = the empty end-of-transmission
 * packet) into dst (RTP_WIRE_MAX_BYTES).  ring[0..have) supplies samples;
 * the rest is silence.  Returns the packet size. */
static inline size_t rtp_wire_build(uint8_t *dst, bool marker, uint16_t seq,
                                    uint32_t ts, uint32_t ssrc,
                                    const int32_t *ring, size_t have, size_t n)
{
    dst[0] = 0x80;
    dst[1] = (uint8_t)((marker ? 0x80 : 0) | RTP_WIRE_PT);
    rtp_wire_wr_be(dst + 2, seq, 2);
    rtp_wire_wr_be(dst + 4, ts, 4);
    rtp_wire_wr_be(dst + 8, ssrc, 4);
    for (size_t i = 0; i < n; i++)
        rtp_wire_ring_to_s16be(dst + RTP_WIRE_HDR_BYTES + 2 * i, i < have ? ring[i] : 0);
    return RTP_WIRE_HDR_BYTES + 2 * n;
}

/* ka9q-radio status TLVs: integers big-endian with leading zero bytes
 * removed (zero -> length 0). */
static inline uint8_t *rtp_wire_tlv_int(uint8_t *p, uint8_t type, uint64_t x)
{
    int len = 8;
    *p++ = type;
    while (len > 0 && (x >> 56) == 0)
    {
        x <<= 8;
        len--;
    }
    *p++ = (uint8_t) len;
    for (int i = 0; i < len; i++, x <<= 8)
        *p++ = (uint8_t)(x >> 56);
    return p;
}

/* Build a status packet describing a PT 125 stream into dst (>= 64 +
 * strlen(desc) bytes).  gps_ns: time of rtp_ts in ns since the GPS epoch.
 * Returns the packet size. */
static inline size_t rtp_wire_build_status(uint8_t *dst, uint32_t ssrc, uint32_t rtp_ts,
                                           uint64_t gps_ns, const char *desc)
{
    size_t dlen = strlen(desc);
    uint8_t *p = dst;

    if (dlen > 127)
        dlen = 127;
    *p++ = 0;                                /* STATUS */
    p = rtp_wire_tlv_int(p, 3, gps_ns);      /* GPS_TIME */
    *p++ = 4;                                /* DESCRIPTION */
    *p++ = (uint8_t) dlen;
    memcpy(p, desc, dlen);
    p += dlen;
    p = rtp_wire_tlv_int(p, 8, rtp_ts);      /* RTP_TIMESNAP */
    p = rtp_wire_tlv_int(p, 18, ssrc);       /* OUTPUT_SSRC */
    p = rtp_wire_tlv_int(p, 20, RTP_WIRE_RATE); /* OUTPUT_SAMPRATE */
    p = rtp_wire_tlv_int(p, 49, 1);          /* OUTPUT_CHANNELS */
    p = rtp_wire_tlv_int(p, 105, RTP_WIRE_PT); /* RTP_PT */
    p = rtp_wire_tlv_int(p, 107, 2);         /* OUTPUT_ENCODING = S16BE */
    *p++ = 0;                                /* EOL */
    return (size_t)(p - dst);
}

#endif /* RTP_WIRE_H_ */
