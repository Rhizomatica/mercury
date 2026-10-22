/* Wire codec for the -x hl2 transport: openHPSDR protocol 1 as the
 * Hermes-Lite 2 speaks it, one receiver at 48 kHz.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Everything runs over UDP to the radio's port 1024:
 *
 *   discovery  EF FE 02 + 60 zero bytes; the reply carries EF FE 02 (free)
 *              or EF FE 03 (streaming to another host), the MAC, the
 *              gateware version and the board id (6 = Hermes-Lite)
 *   start/stop EF FE 04 01 / EF FE 04 00, 64 bytes
 *   EP6 (RX)   1032 bytes: EF FE 01 06, a sequence number, two 512-byte
 *              frames of 7F 7F 7F, five C&C status bytes and 63 samples of
 *              I (24 bit) Q (24 bit) mic (16 bit), all big-endian
 *   EP2 (TX)   1032 bytes: EF FE 01 02, a sequence number, two frames of
 *              7F 7F 7F, five C&C bytes and 63 samples of L, R, I, Q
 *              (16 bit, big-endian); L and R stay 0 on the HL2
 *
 * A C&C block sets one register: C0 = (address << 1) | MOX, C1..C4 its 32
 * bits.  MOX (transmit) rides in every frame.  Frame 1 always carries the
 * configuration register; frame 2 cycles through the rest (HL2_CC_SLOTS).
 *
 * On the wire a signal above the radio's frequency arrives at a NEGATIVE
 * frequency of (first word) + j(second word), the convention of WDSP and so
 * of every host built on it (piHPSDR, deskHPSDR, Thetis): WDSP's USB filter
 * and its CTUN shift both assume it, deskHPSDR's hpsdrsim generates it, and
 * a real HL2 confirms it (an AM carrier tuned 2 kHz below and above, see
 * docs/HERMES-LITE-2.md).
 * This codec conjugates at the boundary (Q = -second word, both ways), so
 * everything above it sees the upper sideband on positive frequencies.
 *
 * References: deskHPSDR's old_protocol.c and hpsdrsim.c, RADE_decode's
 * HermesNetworkManager.kt, the Hermes-Lite 2 wiki.  Pinned by
 * tests/audioio/test_hl2_wire.c.
 */
#ifndef HL2_WIRE_H_
#define HL2_WIRE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define HL2_WIRE_PORT          1024
#define HL2_WIRE_RATE          48000
#define HL2_WIRE_DISC_BYTES    63
#define HL2_WIRE_CMD_BYTES     64
#define HL2_WIRE_PKT_BYTES     1032
#define HL2_WIRE_FRAME_SAMPLES 63
#define HL2_WIRE_PKT_SAMPLES   (2 * HL2_WIRE_FRAME_SAMPLES)   /* 126: 2.625 ms */
#define HL2_WIRE_BOARD_HL      6
#define HL2_WIRE_FREQ_MIN      10000u
#define HL2_WIRE_FREQ_MAX      38400000u
#define HL2_WIRE_LNA_MIN_DB    (-12)
#define HL2_WIRE_LNA_MAX_DB    48
#define HL2_WIRE_PTT_HANG_MS   20     /* deskHPSDR's value */
#define HL2_WIRE_TX_LATENCY_MS 40     /* TX FIFO priming: less is choppy */

/* Frame-2 register slots, one per packet in turn. */
enum {
    HL2_CC_TX_FREQ,     /* 0x01  TX NCO, Hz                                  */
    HL2_CC_RX_FREQ,     /* 0x02  RX1 NCO, Hz                                 */
    HL2_CC_DRIVE,       /* 0x09  drive level, PA enable                      */
    HL2_CC_LNA,         /* 0x0A  RX LNA gain                                 */
    HL2_CC_TX_LNA,      /* 0x0E  LNA gain while transmitting                 */
    HL2_CC_LATENCY,     /* 0x17  PTT hang, TX buffer latency                 */
    HL2_CC_SLOTS
};

typedef struct {
    uint32_t freq_hz;   /* dial frequency (suppressed carrier), RX = TX      */
    uint8_t  drive;     /* 0..255; the HL2 uses the top 4 bits (0.5 dB steps,
                           0 = no RF at all)                                 */
    int      lna_db;    /* -12..+48                                          */
    bool     pa;        /* the 5 W PA; off = the ~10 mW low-power output     */
} hl2_wire_cfg;

typedef struct {
    uint8_t mac[6];
    uint8_t gateware;        /* major; >= 40 is an HL2, below an HL1         */
    uint8_t gateware_minor;  /* 0 if the reply was too short to carry it     */
    uint8_t board_id;
    bool    busy;            /* already streaming to another host            */
} hl2_wire_disc;

/* Radio status from the EP6 C&C bytes; fields keep their last value. */
typedef struct {
    bool     ptt;            /* the radio's own PTT input                    */
    bool     adc_overload;   /* in the last packet                           */
    bool     tx_underflow;   /* TX FIFO ran dry (last report)                */
    bool     tx_overflow;
    uint8_t  firmware;
    uint16_t temp_raw;       /* degC = 0.0795898 * raw - 50                  */
    uint16_t fwd_raw;        /* forward / reverse power ADC                  */
    uint16_t rev_raw;
    uint16_t pa_current_raw; /* mA = 0.505396 * raw                          */
} hl2_wire_status;

static inline uint32_t hl2_wire_rd_be(const uint8_t *p, int n)
{
    uint32_t v = 0;
    for (int i = 0; i < n; i++)
        v = (v << 8) | p[i];
    return v;
}

static inline void hl2_wire_wr_be(uint8_t *p, uint32_t v, int n)
{
    for (int i = n - 1; i >= 0; i--, v >>= 8)
        p[i] = (uint8_t)(v & 0xff);
}

static inline size_t hl2_wire_build_discovery(uint8_t *p)
{
    memset(p, 0, HL2_WIRE_DISC_BYTES);
    p[0] = 0xEF; p[1] = 0xFE; p[2] = 0x02;
    return HL2_WIRE_DISC_BYTES;
}

/* 0 on a discovery reply, -1 otherwise. */
static inline int hl2_wire_parse_discovery(const uint8_t *p, size_t n, hl2_wire_disc *d)
{
    if (n < 11 || p[0] != 0xEF || p[1] != 0xFE || (p[2] != 0x02 && p[2] != 0x03))
        return -1;
    memset(d, 0, sizeof(*d));
    d->busy = p[2] == 0x03;
    memcpy(d->mac, p + 3, 6);
    d->gateware = p[9];
    d->board_id = p[10];
    if (n >= 22)
        d->gateware_minor = p[21];
    return 0;
}

static inline size_t hl2_wire_build_command(uint8_t *p, bool start)
{
    memset(p, 0, HL2_WIRE_CMD_BYTES);
    p[0] = 0xEF; p[1] = 0xFE; p[2] = 0x04; p[3] = start ? 0x01 : 0x00;
    return HL2_WIRE_CMD_BYTES;
}

/* N2ADR filter board (open-collector outputs): one low-pass per band, plus
 * the 3 MHz high-pass on every band above 160 m. */
static inline uint8_t hl2_wire_filter_bits(uint32_t hz)
{
    uint8_t lpf = hz <= 2300000u  ? 0x01 : hz <= 4700000u  ? 0x02 :
                  hz <= 7800000u  ? 0x04 : hz <= 15000000u ? 0x08 :
                  hz <= 22000000u ? 0x10 : 0x20;
    return (uint8_t)(lpf | (hz > 2300000u ? 0x40 : 0x00));
}

static inline uint8_t hl2_wire_lna_code(int db)
{
    if (db < HL2_WIRE_LNA_MIN_DB) db = HL2_WIRE_LNA_MIN_DB;
    if (db > HL2_WIRE_LNA_MAX_DB) db = HL2_WIRE_LNA_MAX_DB;
    return (uint8_t)(0x40 | (db - HL2_WIRE_LNA_MIN_DB));   /* bit 6: 6-bit code */
}

static inline void hl2_wire_set_cc(uint8_t cc[5], uint8_t addr, uint32_t value, bool mox)
{
    cc[0] = (uint8_t)((addr << 1) | (mox ? 1 : 0));
    hl2_wire_wr_be(cc + 1, value, 4);
}

/* Frame 1: the configuration register. 48 kHz, one receiver, duplex (the
 * radio keeps streaming RX while it transmits), the band's filters. */
static inline void hl2_wire_cc_config(uint8_t cc[5], const hl2_wire_cfg *c, bool mox)
{
    uint32_t v = ((uint32_t) hl2_wire_filter_bits(c->freq_hz) << 1) << 16   /* C2 */
               | 0x04u;                                                    /* C4 */
    hl2_wire_set_cc(cc, 0x00, v, mox);
}

/* Frame 2: register `slot` (HL2_CC_*).  While transmitting the receiver's
 * LNA drops to -12 dB, as deskHPSDR does with the PA on. */
static inline void hl2_wire_cc_slot(uint8_t cc[5], int slot, const hl2_wire_cfg *c, bool mox)
{
    uint32_t hz = c->freq_hz;
    if (hz < HL2_WIRE_FREQ_MIN) hz = HL2_WIRE_FREQ_MIN;
    if (hz > HL2_WIRE_FREQ_MAX) hz = HL2_WIRE_FREQ_MAX;

    switch (slot)
    {
    case HL2_CC_TX_FREQ: hl2_wire_set_cc(cc, 0x01, hz, mox); break;
    case HL2_CC_RX_FREQ: hl2_wire_set_cc(cc, 0x02, hz, mox); break;
    case HL2_CC_DRIVE:
        hl2_wire_set_cc(cc, 0x09, (uint32_t) c->drive << 24 | (c->pa ? 0x08u << 16 : 0), mox);
        break;
    case HL2_CC_LNA:
        hl2_wire_set_cc(cc, 0x0A, hl2_wire_lna_code(mox ? HL2_WIRE_LNA_MIN_DB : c->lna_db), mox);
        break;
    case HL2_CC_TX_LNA:     /* bit 7: use this gain while transmitting */
        hl2_wire_set_cc(cc, 0x0E, (uint32_t)(0x80 | hl2_wire_lna_code(HL2_WIRE_LNA_MIN_DB)) << 8, mox);
        break;
    default:                /* HL2_CC_LATENCY */
        hl2_wire_set_cc(cc, 0x17, (uint32_t) HL2_WIRE_PTT_HANG_MS << 8 | HL2_WIRE_TX_LATENCY_MS, mox);
        break;
    }
}

/* One EP2 packet: the two C&C blocks and 126 TX samples (interleaved I, Q,
 * upper sideband on positive frequencies; conjugated onto the wire; NULL =
 * silence).  The low bit of I and Q is cleared: the HL2 gateware
 * reads it as a CW key (deskHPSDR's CWX workaround). */
static inline size_t hl2_wire_build_ep2(uint8_t *p, uint32_t seq, const uint8_t cc1[5],
                                        const uint8_t cc2[5], const int16_t *iq)
{
    memset(p, 0, HL2_WIRE_PKT_BYTES);
    p[0] = 0xEF; p[1] = 0xFE; p[2] = 0x01; p[3] = 0x02;
    hl2_wire_wr_be(p + 4, seq, 4);
    for (int f = 0; f < 2; f++)
    {
        uint8_t *fr = p + 8 + 512 * f;
        fr[0] = fr[1] = fr[2] = 0x7F;
        memcpy(fr + 3, f ? cc2 : cc1, 5);
        if (!iq)
            continue;
        for (int s = 0; s < HL2_WIRE_FRAME_SAMPLES; s++)
        {
            const int16_t *x = iq + 2 * (f * HL2_WIRE_FRAME_SAMPLES + s);
            uint8_t *q = fr + 8 + 8 * s + 4;                  /* after L, R */
            int32_t qn = x[1] == INT16_MIN ? INT16_MAX : -x[1];   /* conjugate */
            hl2_wire_wr_be(q,     (uint16_t)(x[0] & ~1), 2);
            hl2_wire_wr_be(q + 2, (uint16_t)(qn & ~1), 2);
        }
    }
    return HL2_WIRE_PKT_BYTES;
}

static inline void hl2_wire_status_update(hl2_wire_status *st, const uint8_t *cc)
{
    st->ptt = cc[0] & 0x01;
    if (cc[0] & 0x80)                  /* an I2C acknowledgement, no status */
        return;
    switch ((cc[0] >> 3) & 0x1F)
    {
    case 0:
        st->adc_overload |= cc[1] & 0x01;
        st->tx_underflow = (cc[3] & 0xC0) == 0x80;
        st->tx_overflow  = (cc[3] & 0xC0) == 0xC0;
        st->firmware = cc[4];
        break;
    case 1:
        st->temp_raw = (uint16_t) hl2_wire_rd_be(cc + 1, 2);
        st->fwd_raw  = (uint16_t) hl2_wire_rd_be(cc + 3, 2);
        break;
    case 2:
        st->rev_raw        = (uint16_t) hl2_wire_rd_be(cc + 1, 2);
        st->pa_current_raw = (uint16_t) hl2_wire_rd_be(cc + 3, 2);
        break;
    }
}

/* Parse an EP6 packet into 126 I/Q pairs (interleaved, +-1.0 full scale,
 * conjugated: upper sideband on positive frequencies) and the status.  Returns 0, or -1 if it is not an EP6 packet (a frame
 * without its sync is returned as silence). */
static inline int hl2_wire_parse_ep6(const uint8_t *p, size_t n, uint32_t *seq, float *iq,
                                     hl2_wire_status *st)
{
    if (n < HL2_WIRE_PKT_BYTES || p[0] != 0xEF || p[1] != 0xFE || p[2] != 0x01 || p[3] != 0x06)
        return -1;
    *seq = hl2_wire_rd_be(p + 4, 4);
    st->adc_overload = false;
    for (int f = 0; f < 2; f++)
    {
        const uint8_t *fr = p + 8 + 512 * f;
        float *out = iq + 2 * f * HL2_WIRE_FRAME_SAMPLES;
        if (fr[0] != 0x7F || fr[1] != 0x7F || fr[2] != 0x7F)
        {
            memset(out, 0, sizeof(float) * 2 * HL2_WIRE_FRAME_SAMPLES);
            continue;
        }
        hl2_wire_status_update(st, fr + 3);
        for (int s = 0; s < 2 * HL2_WIRE_FRAME_SAMPLES; s++)
        {
            /* I, Q, I, Q, ...: sample s/2 at byte 8 + 8*(s/2), Q 3 bytes on */
            const uint8_t *b = fr + 8 + 8 * (s / 2) + 3 * (s % 2);
            int32_t v = (int32_t)(hl2_wire_rd_be(b, 3) << 8) >> 8;   /* sign-extend */
            out[s] = (float) v * (s % 2 ? -1.0f / 8388608.0f : 1.0f / 8388608.0f);
        }
    }
    return 0;
}

#endif /* HL2_WIRE_H_ */
