/*
 * Monitor-mode frame reporter tests
 *
 * Feeds real protocol frames (built with arq_protocol_build_*) through
 * process_monitor_frame() and pins the MONITOR line it dispatches.  This
 * covers the parsing path -- connect, CQ and broadcast -- without the modem
 * or audio stack.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>

#include "unity.h"

#include "freedv_api.h"    /* FREEDV_MODE_* */
#include "modem_mfsk.h"    /* MERCURY_MODE_MFSK */
#include "framer.h"        /* PACKET_TYPE_*, write_frame_header */
#include "arq.h"           /* ARQ_BANDWIDTH_* */
#include "arq_protocol.h"  /* arq_protocol_build_call / build_cq */
#include "monitor_frame.h" /* process_monitor_frame, mode_name_from_enum */

/* ---- tnc_send_monitor capture stub ---- */

static char cap_mode[32];
static char cap_kind[32];
static char cap_detail[128];
static float cap_snr;
static int  cap_count;

void tnc_send_monitor(const char *mode_name, const char *kind,
                      const char *detail, float snr_db)
{
    snprintf(cap_mode, sizeof(cap_mode), "%s", mode_name ? mode_name : "");
    snprintf(cap_kind, sizeof(cap_kind), "%s", kind ? kind : "");
    snprintf(cap_detail, sizeof(cap_detail), "%s", detail ? detail : "");
    cap_snr = snr_db;
    cap_count++;
}

void setUp(void)
{
    cap_mode[0] = cap_kind[0] = cap_detail[0] = '\0';
    cap_snr = 0.0f;
    cap_count = 0;
}

void tearDown(void) { }

/* process_monitor_frame() reads `data` as the decoded frame plus its trailing
 * 2-byte CRC (nbytes_out counts both), so run_frame() appends a dummy CRC. */
static void run_frame(const uint8_t *frame, size_t frame_len, int mode, float snr)
{
    uint8_t buf[1300];
    TEST_ASSERT_TRUE(frame_len + 2 <= sizeof(buf));
    memcpy(buf, frame, frame_len);
    buf[frame_len]     = 0xAA;
    buf[frame_len + 1] = 0x55;
    process_monitor_frame(buf, frame_len + 2, mode, snr);
}

void test_monitor_call_frame(void)
{
    uint8_t frame[64];
    int n = arq_protocol_build_call(frame, sizeof(frame), 42,
                                    "K7EK", "VK2XYZ", ARQ_BANDWIDTH_FULL_HZ);
    TEST_ASSERT_EQUAL_INT(ARQ_CONTROL_FRAME_SIZE, n);

    run_frame(frame, (size_t)n, FREEDV_MODE_DATAC16, 18.5f);

    TEST_ASSERT_EQUAL_INT(1, cap_count);
    TEST_ASSERT_EQUAL_STRING("DATAC16", cap_mode);
    TEST_ASSERT_EQUAL_STRING("CALL", cap_kind);
    TEST_ASSERT_EQUAL_STRING("FROM=K7EK BW=2300 SID=42", cap_detail);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 18.5f, cap_snr);
}

void test_monitor_cq_frame(void)
{
    uint8_t frame[64];
    int n = arq_protocol_build_cq(frame, sizeof(frame), "K7EK",
                                  ARQ_BANDWIDTH_NARROW_HZ);
    TEST_ASSERT_EQUAL_INT(ARQ_CONTROL_FRAME_SIZE, n);

    run_frame(frame, (size_t)n, FREEDV_MODE_DATAC16, 9.0f);

    TEST_ASSERT_EQUAL_INT(1, cap_count);
    TEST_ASSERT_EQUAL_STRING("DATAC16", cap_mode);
    TEST_ASSERT_EQUAL_STRING("CQ", cap_kind);
    TEST_ASSERT_EQUAL_STRING("FROM=K7EK BW=500", cap_detail);
}

void test_monitor_broadcast_frame(void)
{
    uint8_t frame[32];
    memset(frame, 0, sizeof(frame));
    write_frame_header(frame, PACKET_TYPE_BROADCAST_DATA, 0);
    for (int i = 1; i < 10; i++)
        frame[i] = (uint8_t)i;

    run_frame(frame, 10, FREEDV_MODE_DATAC15, 12.0f);

    TEST_ASSERT_EQUAL_INT(1, cap_count);
    TEST_ASSERT_EQUAL_STRING("DATAC15", cap_mode);
    TEST_ASSERT_EQUAL_STRING("BCAST_DATA", cap_kind);
    TEST_ASSERT_EQUAL_STRING("LEN=10", cap_detail);
}

void test_mode_name_from_enum(void)
{
    TEST_ASSERT_EQUAL_STRING("DATAC16", mode_name_from_enum(FREEDV_MODE_DATAC16));
    TEST_ASSERT_EQUAL_STRING("QAM16C2", mode_name_from_enum(FREEDV_MODE_QAM16C2));
    TEST_ASSERT_EQUAL_STRING("MFSK", mode_name_from_enum(MERCURY_MODE_MFSK));
    TEST_ASSERT_EQUAL_STRING("UNKNOWN", mode_name_from_enum(-1));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_monitor_call_frame);
    RUN_TEST(test_monitor_cq_frame);
    RUN_TEST(test_monitor_broadcast_frame);
    RUN_TEST(test_mode_name_from_enum);
    return UNITY_END();
}
