/*
 * Wire-codec tests for the -x hl2 transport (hl2_wire.h): openHPSDR
 * protocol 1 as the Hermes-Lite 2 speaks it.
 *
 * Expected bytes are hand-derived from deskHPSDR's old_protocol.c /
 * old_discovery.c and RADE_decode's HermesNetworkManager.kt, so a change on
 * our side of the wire fails here.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "unity.h"
#include "hl2_wire.h"

#include <stdint.h>
#include <string.h>

void setUp(void)    { }
void tearDown(void) { }

static const hl2_wire_cfg CFG = {
    .freq_hz = 7050000, .drive = 0xA0, .lna_db = 20, .pa = true,
};

void test_discovery_request_and_reply(void)
{
    uint8_t p[HL2_WIRE_DISC_BYTES];
    TEST_ASSERT_EQUAL_size_t(63, hl2_wire_build_discovery(p));
    TEST_ASSERT_EQUAL_HEX8(0xEF, p[0]);
    TEST_ASSERT_EQUAL_HEX8(0xFE, p[1]);
    TEST_ASSERT_EQUAL_HEX8(0x02, p[2]);
    for (int i = 3; i < 63; i++)
        TEST_ASSERT_EQUAL_HEX8(0, p[i]);

    uint8_t r[60] = { 0xEF, 0xFE, 0x02, 0x00, 0x1C, 0xC0, 0xA2, 0x13, 0xDD, 73, 6 };
    r[21] = 2;
    hl2_wire_disc d;
    TEST_ASSERT_EQUAL_INT(0, hl2_wire_parse_discovery(r, sizeof(r), &d));
    TEST_ASSERT_FALSE(d.busy);
    TEST_ASSERT_EQUAL_HEX8(0x1C, d.mac[1]);
    TEST_ASSERT_EQUAL_HEX8(0xDD, d.mac[5]);
    TEST_ASSERT_EQUAL_UINT8(73, d.gateware);
    TEST_ASSERT_EQUAL_UINT8(2, d.gateware_minor);
    TEST_ASSERT_EQUAL_UINT8(HL2_WIRE_BOARD_HL, d.board_id);

    r[2] = 0x03;                                   /* streaming elsewhere */
    TEST_ASSERT_EQUAL_INT(0, hl2_wire_parse_discovery(r, 11, &d));
    TEST_ASSERT_TRUE(d.busy);
    TEST_ASSERT_EQUAL_UINT8(0, d.gateware_minor);  /* too short to carry it */

    r[2] = 0x01;                                   /* a data packet */
    TEST_ASSERT_EQUAL_INT(-1, hl2_wire_parse_discovery(r, sizeof(r), &d));
    r[2] = 0x02;
    TEST_ASSERT_EQUAL_INT(-1, hl2_wire_parse_discovery(r, 10, &d));
}

void test_start_and_stop(void)
{
    uint8_t p[HL2_WIRE_CMD_BYTES];
    TEST_ASSERT_EQUAL_size_t(64, hl2_wire_build_command(p, true));
    const uint8_t start[] = { 0xEF, 0xFE, 0x04, 0x01, 0x00 };
    TEST_ASSERT_EQUAL_HEX8_ARRAY(start, p, 5);
    hl2_wire_build_command(p, false);
    TEST_ASSERT_EQUAL_HEX8(0x00, p[3]);
    TEST_ASSERT_EQUAL_HEX8(0x00, p[63]);
}

void test_config_register(void)
{
    uint8_t cc[5];
    hl2_wire_cc_config(cc, &CFG, false);
    /* 48 kHz; 40 m low-pass 0x04 + high-pass 0x40, << 1; duplex, 1 RX */
    const uint8_t want[] = { 0x00, 0x00, 0x88, 0x00, 0x04 };
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, cc, 5);
    hl2_wire_cc_config(cc, &CFG, true);
    TEST_ASSERT_EQUAL_HEX8(0x01, cc[0]);           /* MOX */

    /* RX rate in C1 bits 1:0; 0 and anything unknown mean 48 kHz */
    const uint32_t rates[] = { 0, 48000, 96000, 192000, 384000, 44100 };
    const uint8_t bits[]   = { 0, 0,     1,     2,      3,      0 };
    for (int i = 0; i < 6; i++)
    {
        hl2_wire_cfg c = CFG;
        c.rx_rate = rates[i];
        hl2_wire_cc_config(cc, &c, false);
        TEST_ASSERT_EQUAL_HEX8(bits[i], cc[1]);
        TEST_ASSERT_EQUAL_HEX8(0x88, cc[2]);
    }
}

void test_filter_bits_per_band(void)
{
    TEST_ASSERT_EQUAL_HEX8(0x01, hl2_wire_filter_bits(1840000));   /* 160 m: no HPF */
    TEST_ASSERT_EQUAL_HEX8(0x42, hl2_wire_filter_bits(3573000));
    TEST_ASSERT_EQUAL_HEX8(0x44, hl2_wire_filter_bits(7074000));
    TEST_ASSERT_EQUAL_HEX8(0x48, hl2_wire_filter_bits(14074000));
    TEST_ASSERT_EQUAL_HEX8(0x50, hl2_wire_filter_bits(21074000));
    TEST_ASSERT_EQUAL_HEX8(0x60, hl2_wire_filter_bits(28074000));
}

void test_slot_registers(void)
{
    uint8_t cc[5];

    hl2_wire_cc_slot(cc, HL2_CC_TX_FREQ, &CFG, false);   /* 7050000 = 0x006B9310 */
    const uint8_t tx[] = { 0x02, 0x00, 0x6B, 0x93, 0x10 };
    TEST_ASSERT_EQUAL_HEX8_ARRAY(tx, cc, 5);

    hl2_wire_cc_slot(cc, HL2_CC_RX_FREQ, &CFG, true);
    const uint8_t rx[] = { 0x05, 0x00, 0x6B, 0x93, 0x10 };
    TEST_ASSERT_EQUAL_HEX8_ARRAY(rx, cc, 5);

    hl2_wire_cc_slot(cc, HL2_CC_DRIVE, &CFG, false);     /* drive, PA bit 19 */
    const uint8_t dr[] = { 0x12, 0xA0, 0x08, 0x00, 0x00 };
    TEST_ASSERT_EQUAL_HEX8_ARRAY(dr, cc, 5);
    hl2_wire_cfg nopa = CFG;
    nopa.pa = false;
    hl2_wire_cc_slot(cc, HL2_CC_DRIVE, &nopa, false);
    TEST_ASSERT_EQUAL_HEX8(0x00, cc[2]);

    hl2_wire_cc_slot(cc, HL2_CC_LNA, &CFG, false);       /* 0x40 | (20 + 12) */
    const uint8_t lna[] = { 0x14, 0x00, 0x00, 0x00, 0x60 };
    TEST_ASSERT_EQUAL_HEX8_ARRAY(lna, cc, 5);
    hl2_wire_cc_slot(cc, HL2_CC_LNA, &CFG, true);        /* -12 dB on TX */
    TEST_ASSERT_EQUAL_HEX8(0x40, cc[4]);

    hl2_wire_cc_slot(cc, HL2_CC_TX_LNA, &CFG, false);
    const uint8_t txl[] = { 0x1C, 0x00, 0x00, 0xC0, 0x00 };
    TEST_ASSERT_EQUAL_HEX8_ARRAY(txl, cc, 5);

    hl2_wire_cc_slot(cc, HL2_CC_LATENCY, &CFG, false);   /* hang 20, latency 40 */
    const uint8_t lat[] = { 0x2E, 0x00, 0x00, 20, 40 };
    TEST_ASSERT_EQUAL_HEX8_ARRAY(lat, cc, 5);
}

void test_lna_and_frequency_clamped(void)
{
    TEST_ASSERT_EQUAL_HEX8(0x40, hl2_wire_lna_code(-30));
    TEST_ASSERT_EQUAL_HEX8(0x40 | 60, hl2_wire_lna_code(99));
    hl2_wire_cfg c = CFG;
    c.freq_hz = 60000000;
    uint8_t cc[5];
    hl2_wire_cc_slot(cc, HL2_CC_RX_FREQ, &c, false);
    TEST_ASSERT_EQUAL_UINT32(HL2_WIRE_FREQ_MAX, hl2_wire_rd_be(cc + 1, 4));
}

void test_ep2_layout(void)
{
    uint8_t p[HL2_WIRE_PKT_BYTES], c1[5], c2[5];
    int16_t iq[2 * HL2_WIRE_PKT_SAMPLES];
    for (int i = 0; i < HL2_WIRE_PKT_SAMPLES; i++)
    {
        iq[2 * i] = (int16_t)(0x1235 + i);
        iq[2 * i + 1] = (int16_t)(-1 - i);
    }
    hl2_wire_cc_config(c1, &CFG, true);
    hl2_wire_cc_slot(c2, HL2_CC_LATENCY, &CFG, true);
    TEST_ASSERT_EQUAL_size_t(1032, hl2_wire_build_ep2(p, 0x01020304u, c1, c2, iq));

    const uint8_t hdr[] = { 0xEF, 0xFE, 0x01, 0x02, 0x01, 0x02, 0x03, 0x04,
                            0x7F, 0x7F, 0x7F, 0x01, 0x00, 0x88, 0x00, 0x04 };
    TEST_ASSERT_EQUAL_HEX8_ARRAY(hdr, p, sizeof(hdr));
    /* first sample: L R = 0, I 0x1235 -> 0x1234 (CWX bit cleared), Q -1 goes
     * out conjugated, +1 -> 0x0000 */
    const uint8_t s0[] = { 0, 0, 0, 0, 0x12, 0x34, 0x00, 0x00 };
    TEST_ASSERT_EQUAL_HEX8_ARRAY(s0, p + 16, 8);
    /* frame 2 sync and C&C at 520, its first sample is sample 63 */
    const uint8_t f2[] = { 0x7F, 0x7F, 0x7F, 0x2F, 0x00, 0x00, 20, 40 };
    TEST_ASSERT_EQUAL_HEX8_ARRAY(f2, p + 520, 8);
    TEST_ASSERT_EQUAL_HEX8(0x12, p[528 + 4]);
    TEST_ASSERT_EQUAL_HEX8((uint8_t)((0x1235 + 63) & 0xFE), p[528 + 5]);

    iq[1] = -0x1235;                                     /* conjugate: +0x1234 */
    iq[3] = INT16_MIN;                                   /* -(-32768) saturates */
    hl2_wire_build_ep2(p, 0, c1, c2, iq);
    TEST_ASSERT_EQUAL_HEX8(0x12, p[22]);
    TEST_ASSERT_EQUAL_HEX8(0x34, p[23]);
    TEST_ASSERT_EQUAL_HEX8(0x7F, p[30]);
    TEST_ASSERT_EQUAL_HEX8(0xFE, p[31]);

    hl2_wire_build_ep2(p, 7, c1, c2, NULL);              /* silence */
    for (int i = 16; i < 520; i++)
        TEST_ASSERT_EQUAL_HEX8(0, p[i]);
}

/* Build an EP6 packet by hand: 24-bit samples sign-extend, the status
 * registers land in their fields, a frame without sync is silence. */
void test_ep6_parse(void)
{
    uint8_t p[HL2_WIRE_PKT_BYTES];
    memset(p, 0, sizeof(p));
    p[0] = 0xEF; p[1] = 0xFE; p[2] = 0x01; p[3] = 0x06;
    hl2_wire_wr_be(p + 4, 77, 4);
    for (int f = 0; f < 2; f++)
        p[8 + 512 * f] = p[9 + 512 * f] = p[10 + 512 * f] = 0x7F;
    /* frame 1 status addr 1: temperature 0x0321, forward power 0x0456 */
    p[11] = 1 << 3; p[12] = 0x03; p[13] = 0x21; p[14] = 0x04; p[15] = 0x56;
    /* sample 0: I = +0x400000 (0.5), second word -0x400000: Q = +0.5 */
    p[16] = 0x40; p[19] = 0xC0;
    /* frame 2 status addr 0: ADC overload, TX underflow, firmware 73; PTT */
    p[523] = 0x01; p[524] = 0x01; p[526] = 0x80; p[527] = 73;
    /* frame 2 sample 0 = packet sample 63: I = -1 LSB */
    p[528] = 0xFF; p[529] = 0xFF; p[530] = 0xFF;

    uint32_t seq = 0;
    float iq[2 * HL2_WIRE_PKT_SAMPLES];
    hl2_wire_status st = { 0 };
    TEST_ASSERT_EQUAL_INT(0, hl2_wire_parse_ep6(p, sizeof(p), &seq, iq, &st));
    TEST_ASSERT_EQUAL_UINT32(77, seq);
    TEST_ASSERT_EQUAL_FLOAT(0.5f, iq[0]);
    TEST_ASSERT_EQUAL_FLOAT(0.5f, iq[1]);
    TEST_ASSERT_EQUAL_FLOAT(-1.0f / 8388608.0f, iq[2 * 63]);
    TEST_ASSERT_EQUAL_UINT16(0x0321, st.temp_raw);
    TEST_ASSERT_EQUAL_UINT16(0x0456, st.fwd_raw);
    TEST_ASSERT_TRUE(st.adc_overload);
    TEST_ASSERT_TRUE(st.tx_underflow);
    TEST_ASSERT_FALSE(st.tx_overflow);
    TEST_ASSERT_EQUAL_UINT8(73, st.firmware);
    TEST_ASSERT_TRUE(st.ptt);

    p[520] = 0x00;                                        /* frame 2 lost sync */
    p[528] = 0x40;
    TEST_ASSERT_EQUAL_INT(0, hl2_wire_parse_ep6(p, sizeof(p), &seq, iq, &st));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, iq[2 * 63]);
    TEST_ASSERT_FALSE(st.adc_overload);                   /* per packet */

    p[3] = 0x04;                                          /* wideband: not ours */
    TEST_ASSERT_EQUAL_INT(-1, hl2_wire_parse_ep6(p, sizeof(p), &seq, iq, &st));
    p[3] = 0x06;
    TEST_ASSERT_EQUAL_INT(-1, hl2_wire_parse_ep6(p, 1031, &seq, iq, &st));
}

/* An I2C acknowledgement frame carries PTT but no status register. */
void test_ep6_i2c_ack_is_not_status(void)
{
    hl2_wire_status st = { .temp_raw = 5 };
    const uint8_t ack[5] = { 0x80 | (1 << 3), 0xF1, 0xF1, 0xF1, 0xF1 };
    hl2_wire_status_update(&st, ack);
    TEST_ASSERT_EQUAL_UINT16(5, st.temp_raw);
    TEST_ASSERT_FALSE(st.ptt);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_discovery_request_and_reply);
    RUN_TEST(test_start_and_stop);
    RUN_TEST(test_config_register);
    RUN_TEST(test_filter_bits_per_band);
    RUN_TEST(test_slot_registers);
    RUN_TEST(test_lna_and_frequency_clamped);
    RUN_TEST(test_ep2_layout);
    RUN_TEST(test_ep6_parse);
    RUN_TEST(test_ep6_i2c_ack_is_not_status);
    return UNITY_END();
}
