/*
 * TCP TNC Interface Unit Tests
 *
 * Tests for data_interfaces/tcp_interfaces.c — VARA-compatible command
 * parsing (execute_control_command) and status emitter formatting.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <stdbool.h>
#include <string.h>
#include <stdint.h>
#include <stdatomic.h>
#include <sys/types.h>
#include <unistd.h>
#include <poll.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>

/* Project headers for type declarations */
#include "os_interop.h"
#include "ring_buffer_posix.h"
#include "net.h"
#include "arq.h"
#include "chan.h"
#include "defines_modem.h"
#include "kiss.h"
#include "hermes_log.h"
#include "radio_io.h"

/* include source to get access to static functions */
#include "../../data_interfaces/tcp_interfaces.c"

#include "unity.h"

/* ---- hermes_log stubs ---- */

void hermes_logf(hermes_log_level_t level, const char *component,
                 const char *fmt, ...)
{
    (void)level; (void)component; (void)fmt;
}

uint64_t hermes_uptime_ms(void) { return 1000; }

/* ---- arq stubs ---- */

static arq_cmd_msg_t captured_cmd;
static int captured_cmd_count = 0;
static int arq_submit_return = 0;

int arq_submit_tcp_cmd(const arq_cmd_msg_t *cmd)
{
    memcpy(&captured_cmd, cmd, sizeof(arq_cmd_msg_t));
    captured_cmd_count++;
    return arq_submit_return;
}

int arq_submit_tcp_payload(const uint8_t *data, size_t len)
{
    (void)data; (void)len;
    return 0;
}

static arq_runtime_snapshot_t mock_snapshot;
static bool mock_snapshot_valid = true;

bool arq_get_runtime_snapshot(arq_runtime_snapshot_t *snapshot)
{
    if (mock_snapshot_valid && snapshot)
        memcpy(snapshot, &mock_snapshot, sizeof(*snapshot));
    return mock_snapshot_valid;
}

static int mock_bandwidth_hz = 2300;

int arq_reported_bandwidth_hz(void)
{
    return mock_bandwidth_hz;
}

bool arq_bandwidth_allows_mode(int mode) { (void)mode; return true; }

/* ---- net stubs ---- */

int cli_ctl_sockfd = -1;
int cli_data_sockfd = -1;
atomic_int status_ctl = 0;
atomic_int status_data = 0;

static uint8_t last_tcp_write_buf[256];
static size_t last_tcp_write_len = 0;
static int tcp_write_call_count = 0;

ssize_t tcp_write(int port_type, uint8_t *buffer, size_t tx_size)
{
    (void)port_type;
    if (tx_size < sizeof(last_tcp_write_buf)) {
        memcpy(last_tcp_write_buf, buffer, tx_size);
        last_tcp_write_buf[tx_size] = '\0';
        last_tcp_write_len = tx_size;
    }
    tcp_write_call_count++;
    return (ssize_t)tx_size;
}

void net_set_status(int pt, int st) { (void)pt; (void)st; }
int net_get_status(int pt) { (void)pt; return NET_CONNECTED; }
int net_wait_for_status(int p, int s, int t) { (void)p; (void)s; (void)t; return 0; }
int net_wait_while_status(int p, int s, int t) { (void)p; (void)s; (void)t; return 0; }
int listen4connection(int p) { (void)p; return 0; }
int tcp_open(int p, int pt) { (void)p; (void)pt; return 0; }
ssize_t tcp_read(int pt, uint8_t *b, size_t s) { (void)pt; (void)b; (void)s; return 0; }
int tcp_close(int pt) { (void)pt; return 0; }

/* ---- radio_io stubs ---- */

bool radio_io_enabled(void) { return false; }
void radio_io_key_on(void) { }
void radio_io_key_off(void) { }

/* ---- ring_buffer stubs ---- */

size_t size_buffer(cbuf_handle_t cbuf) { (void)cbuf; return 0; }
int read_buffer(cbuf_handle_t cbuf, uint8_t *data, size_t len) { (void)cbuf; (void)data; (void)len; return 0; }
void clear_buffer(cbuf_handle_t cbuf) { (void)cbuf; }

static uint8_t last_write_buffer_data[MAX_PAYLOAD];
static size_t  last_write_buffer_len  = 0;
static int     write_buffer_call_count = 0;

int write_buffer(cbuf_handle_t cbuf, uint8_t *data, size_t len)
{
    (void)cbuf;
    write_buffer_call_count++;
    if (len <= MAX_PAYLOAD)
    {
        memcpy(last_write_buffer_data, data, len);
        last_write_buffer_len = len;
    }
    return 0;
}

/* ---- kiss stubs ---- */

int kiss_write_frame(uint8_t *a, int b, uint8_t cmd, uint8_t *c) { (void)a; (void)b; (void)cmd; (void)c; return 0; }
int kiss_read(uint8_t b, uint8_t *c) { (void)b; (void)c; return 0; }
void kiss_reset_state(void) { }

static uint8_t mock_kiss_last_command_val = CMD_DATA;
uint8_t kiss_last_command(void) { return mock_kiss_last_command_val; }

/* ---- chan stubs ---- */

static char last_queued_line[256];
static int chan_select_call_count = 0;

chan_t *chan_init(size_t capacity)
{
    (void)capacity;
    static chan_t dummy;
    return &dummy;
}

void chan_dispose(chan_t *chan) { (void)chan; }
int chan_close(chan_t *chan) { (void)chan; return 0; }

int chan_select(chan_t *recv_chans[], int recv_count, void **recv_out,
               chan_t *send_chans[], int send_count, void *send_msgs[])
{
    (void)recv_chans; (void)recv_count; (void)recv_out;
    (void)send_chans;

    chan_select_call_count++;

    /* Capture the tnc_tx_msg_t data for status emitter tests.
     * tcp_interfaces.c defines tnc_tx_msg_t locally, but since we'll
     * #include it, the type will be visible at this point only AFTER
     * the include. For now, just capture raw bytes. */
    if (send_count > 0 && send_msgs && send_msgs[0]) {
        /* tnc_tx_msg_t layout: { size_t len; uint8_t data[128]; } */
        size_t len;
        memcpy(&len, send_msgs[0], sizeof(size_t));
        uint8_t *data_ptr = (uint8_t *)send_msgs[0] + sizeof(size_t);
        memset(last_queued_line, 0, sizeof(last_queued_line));
        if (len < sizeof(last_queued_line))
            memcpy(last_queued_line, data_ptr, len);
        /* Returning 0 signals the message was accepted by the channel, so
         * tnc_queue_line() transfers ownership and does not free it (in
         * production the send_thread consumer frees each msg).  This mock is
         * that terminal consumer, so it must free the msg or it leaks — which
         * LeakSanitizer flags in the tnc_send_* tests. */
        free(send_msgs[0]);
        return 0;
    }

    return -1;
}

/* ---- External globals used by tcp_interfaces.c ---- */

cbuf_handle_t data_tx_buffer_arq = NULL;
cbuf_handle_t data_rx_buffer_arq = NULL;
cbuf_handle_t data_tx_buffer_broadcast = NULL;
cbuf_handle_t data_rx_buffer_broadcast = NULL;

volatile bool shutdown_ = false;
arq_info arq_conn = {0};

/* ---- setUp / tearDown ---- */

void setUp(void)
{
    memset(last_tcp_write_buf, 0, sizeof(last_tcp_write_buf));
    last_tcp_write_len = 0;
    tcp_write_call_count = 0;
    memset(&captured_cmd, 0, sizeof(captured_cmd));
    captured_cmd_count = 0;
    arq_submit_return = 0;
    memset(last_queued_line, 0, sizeof(last_queued_line));
    chan_select_call_count = 0;
    memset(&arq_conn, 0, sizeof(arq_conn));
    mock_bandwidth_hz = 2300;

    /* Broadcast framing state */
    memset(last_write_buffer_data, 0, sizeof(last_write_buffer_data));
    last_write_buffer_len  = 0;
    write_buffer_call_count = 0;
    mock_kiss_last_command_val = CMD_DATA;
    atomic_store_explicit(&bcast_reply_cmd, CMD_DATA, memory_order_relaxed);

    /* tnc_queue_line() needs tnc_tx_chan non-NULL */
    static chan_t dummy_chan;
    tnc_tx_chan = &dummy_chan;

    /* Reset dedup */
    atomic_store_explicit(&tnc_last_buffer_sent, -1, memory_order_relaxed);
}

void tearDown(void) { }

/* ---- Helpers ---- */

static void assert_ok_response(void)
{
    TEST_ASSERT_EQUAL(1, tcp_write_call_count);
    TEST_ASSERT_EQUAL_STRING_LEN("OK\r", (char *)last_tcp_write_buf, 3);
}

static void assert_wrong_response(void)
{
    TEST_ASSERT_EQUAL(1, tcp_write_call_count);
    TEST_ASSERT_EQUAL_STRING_LEN("WRONG\r", (char *)last_tcp_write_buf, 6);
}

/* ---- Command parser tests ---- */

void test_cmd_mycall(void)
{
    char cmd[] = "MYCALL TEST1";
    execute_control_command(cmd);

    assert_ok_response();
    TEST_ASSERT_EQUAL_INT(ARQ_CMD_SET_CALLSIGN, captured_cmd.type);
    TEST_ASSERT_EQUAL_STRING("TEST1", captured_cmd.arg0);
}

void test_cmd_listen_on(void)
{
    char cmd[] = "LISTEN ON";
    execute_control_command(cmd);

    assert_ok_response();
    TEST_ASSERT_EQUAL_INT(ARQ_CMD_LISTEN_ON, captured_cmd.type);
}

void test_cmd_listen_off(void)
{
    char cmd[] = "LISTEN OFF";
    execute_control_command(cmd);

    assert_ok_response();
    TEST_ASSERT_EQUAL_INT(ARQ_CMD_LISTEN_OFF, captured_cmd.type);
}

void test_cmd_public_on(void)
{
    char cmd[] = "PUBLIC ON";
    execute_control_command(cmd);

    assert_ok_response();
    TEST_ASSERT_EQUAL_INT(ARQ_CMD_SET_PUBLIC, captured_cmd.type);
    TEST_ASSERT_TRUE(captured_cmd.flag);
}

void test_cmd_compression(void)
{
    char cmd[] = "COMPRESSION TEXT";
    execute_control_command(cmd);

    assert_ok_response();
    TEST_ASSERT_EQUAL(0, captured_cmd_count);
}

void test_cmd_chat_on(void)
{
    char cmd[] = "CHAT ON";
    execute_control_command(cmd);

    assert_ok_response();
    TEST_ASSERT_EQUAL_INT(ARQ_CMD_LISTEN_ON, captured_cmd.type);
}

void test_cmd_bw500(void)
{
    char cmd[] = "BW500";
    execute_control_command(cmd);

    assert_ok_response();
    TEST_ASSERT_EQUAL_INT(ARQ_CMD_SET_BANDWIDTH, captured_cmd.type);
    TEST_ASSERT_EQUAL_INT(500, captured_cmd.value);
}

void test_cmd_bw2300(void)
{
    char cmd[] = "BW2300";
    execute_control_command(cmd);

    assert_ok_response();
    TEST_ASSERT_EQUAL_INT(ARQ_CMD_SET_BANDWIDTH, captured_cmd.type);
    TEST_ASSERT_EQUAL_INT(2300, captured_cmd.value);
}

void test_cmd_bw_invalid(void)
{
    char cmd[] = "BW1234";
    execute_control_command(cmd);

    assert_wrong_response();
}

void test_cmd_retries(void)
{
    char cmd[] = "RETRIES 10";
    execute_control_command(cmd);

    assert_ok_response();
    TEST_ASSERT_EQUAL_INT(ARQ_CMD_SET_RETRY, captured_cmd.type);
    TEST_ASSERT_EQUAL_INT(10, captured_cmd.value);
}

void test_cmd_connect(void)
{
    char cmd[] = "CONNECT SRC1 DST1";
    execute_control_command(cmd);

    assert_ok_response();
    TEST_ASSERT_EQUAL_INT(ARQ_CMD_CONNECT, captured_cmd.type);
    TEST_ASSERT_EQUAL_STRING("SRC1", captured_cmd.arg0);
    TEST_ASSERT_EQUAL_STRING("DST1", captured_cmd.arg1);
}

void test_cmd_disconnect(void)
{
    char cmd[] = "DISCONNECT";
    execute_control_command(cmd);

    assert_ok_response();
    TEST_ASSERT_EQUAL_INT(ARQ_CMD_DISCONNECT, captured_cmd.type);
}

void test_cmd_cqframe(void)
{
    char cmd[] = "CQFRAME CALL1 2300";
    execute_control_command(cmd);

    assert_ok_response();
    TEST_ASSERT_EQUAL_INT(ARQ_CMD_SEND_CQ, captured_cmd.type);
    TEST_ASSERT_EQUAL_STRING("CALL1", captured_cmd.arg0);
    TEST_ASSERT_EQUAL_INT(2300, captured_cmd.value);
}

void test_cmd_p2p(void)
{
    char cmd[] = "P2P";
    execute_control_command(cmd);

    assert_ok_response();
}

void test_cmd_unknown(void)
{
    char cmd[] = "FOOBAR";
    execute_control_command(cmd);

    assert_wrong_response();
}

void test_cmd_submit_failure(void)
{
    arq_submit_return = -1;
    char cmd[] = "DISCONNECT";
    execute_control_command(cmd);

    assert_wrong_response();
}

void test_process_control_bytes_multiline(void)
{
    char line_buf[TCP_BLOCK_SIZE + 1] = {0};
    int line_len = 0;

    const uint8_t data[] = "MYCALL A\rP2P\r";
    process_control_bytes(line_buf, &line_len, data, (ssize_t)(sizeof(data) - 1));

    TEST_ASSERT_EQUAL(1, captured_cmd_count);
    TEST_ASSERT_EQUAL(2, tcp_write_call_count);
}

/* ---- Status emitter tests ---- */

void test_tnc_send_disconnected(void)
{
    tnc_send_disconnected();
    TEST_ASSERT_EQUAL_STRING("DISCONNECTED\r", last_queued_line);
}

void test_tnc_send_pending(void)
{
    tnc_send_pending();
    TEST_ASSERT_EQUAL_STRING("PENDING\r", last_queued_line);
}

void test_tnc_send_cancelpending(void)
{
    tnc_send_cancelpending();
    TEST_ASSERT_EQUAL_STRING("CANCELPENDING\r", last_queued_line);
}

void test_tnc_send_buffer(void)
{
    tnc_send_buffer(1234);
    TEST_ASSERT_EQUAL_STRING("BUFFER 1234\r", last_queued_line);
}

void test_tnc_send_sn(void)
{
    tnc_send_sn(5.5f);
    TEST_ASSERT_EQUAL_STRING("SN 5.5\r", last_queued_line);
}

void test_tnc_send_bitrate(void)
{
    tnc_send_bitrate(2, 600);
    TEST_ASSERT_EQUAL_STRING("BITRATE (2) 600 BPS\r", last_queued_line);
}

void test_tnc_send_connected(void)
{
    strncpy(arq_conn.src_addr, "SRC1", CALLSIGN_MAX_SIZE);
    strncpy(arq_conn.dst_addr, "DST1", CALLSIGN_MAX_SIZE);
    mock_bandwidth_hz = 2300;

    tnc_send_connected();
    TEST_ASSERT_EQUAL_STRING("CONNECTED SRC1 DST1 2300\r", last_queued_line);
}

void test_tnc_send_cqframe(void)
{
    tnc_send_cqframe("CALL1", 500);
    TEST_ASSERT_EQUAL_STRING("CQFRAME CALL1 500\r", last_queued_line);
}

/* ---- VARA compatibility command tests ---- */

void test_cmd_abort(void)
{
    char cmd[] = "ABORT";
    execute_control_command(cmd);

    assert_ok_response();
    TEST_ASSERT_EQUAL_INT(ARQ_CMD_ABORT, captured_cmd.type);
}

void test_cmd_abort_submit_failure(void)
{
    arq_submit_return = -1;
    char cmd[] = "ABORT";
    execute_control_command(cmd);

    assert_wrong_response();
}

void test_cmd_version(void)
{
    char cmd[] = "VERSION";
    execute_control_command(cmd);

    TEST_ASSERT_EQUAL(1, tcp_write_call_count);
    TEST_ASSERT_EQUAL_STRING_LEN("VARA version 4.9.0 registered\r",
                                 (char *)last_tcp_write_buf, 30);
    /* VERSION must not submit any ARQ command */
    TEST_ASSERT_EQUAL(0, captured_cmd_count);
}

void test_cmd_ignorekissdcd(void)
{
    char cmd[] = "IGNOREKISSDCD ON";
    execute_control_command(cmd);

    assert_ok_response();
    /* Must not submit any ARQ command */
    TEST_ASSERT_EQUAL(0, captured_cmd_count);
}

void test_cmd_listen_cq(void)
{
    char cmd[] = "LISTEN CQ";
    execute_control_command(cmd);

    assert_ok_response();
    TEST_ASSERT_EQUAL_INT(ARQ_CMD_LISTEN_ON, captured_cmd.type);
}

/* ---- CALLINT command tests ---- */

void test_cmd_callint_valid(void)
{
    char cmd[] = "CALLINT 5";
    execute_control_command(cmd);

    assert_ok_response();
    TEST_ASSERT_EQUAL_INT(ARQ_CMD_SET_CALLINT, captured_cmd.type);
    TEST_ASSERT_EQUAL_INT(5, captured_cmd.value);
}

void test_cmd_callint_zero(void)
{
    char cmd[] = "CALLINT 0";
    execute_control_command(cmd);

    assert_ok_response();
    TEST_ASSERT_EQUAL_INT(ARQ_CMD_SET_CALLINT, captured_cmd.type);
    TEST_ASSERT_EQUAL_INT(0, captured_cmd.value);
}

void test_cmd_callint_no_arg(void)
{
    char cmd[] = "CALLINT";
    execute_control_command(cmd);

    assert_wrong_response();
}

void test_cmd_callint_nonnumeric(void)
{
    char cmd[] = "CALLINT abc";
    execute_control_command(cmd);

    assert_wrong_response();
}

void test_cmd_callint_negative(void)
{
    char cmd[] = "CALLINT -1";
    execute_control_command(cmd);

    assert_wrong_response();
}

/* ---- Broadcast framing helper tests ---- */

/* Expected Mercury header byte for PACKET_TYPE_BROADCAST_DATA (0x04), ext=0:
 *   (0x04 << 5) | 0 = 0x80 */
#define BCAST_HDR_BYTE 0x80

/* Length-prefixed broadcast framing (mirrors tcp_interfaces.c). VARA frames now
 * carry a 2-byte length after the header, flagged with ext bit 0x01, so the
 * header byte becomes 0x80 | 0x01 = 0x81. */
#define BCAST_LEN_SIZE       2
#define BCAST_EXT_LEN_PREFIX 0x01
#define BCAST_EXT_KISS_STD   0x02
#define BCAST_HDR_BYTE_LEN   (BCAST_HDR_BYTE | BCAST_EXT_LEN_PREFIX)
#define BCAST_HDR_BYTE_STD   (BCAST_HDR_BYTE | BCAST_EXT_LEN_PREFIX | BCAST_EXT_KISS_STD)

/* CMD_DATA, exact frame_size: queued unchanged, bcast_reply_cmd = CMD_DATA */
void test_bcast_rx_cmd_data_exact_size(void)
{
    const size_t fsz = 10;
    broadcast_frame_size_cfg = fsz;

    uint8_t frame[MAX_PAYLOAD];
    memset(frame, 0xAA, fsz);
    frame[0] = 0x60; /* arbitrary Mercury header already in place */

    bool ok = bcast_process_decoded_frame(frame, (int)fsz, CMD_DATA, fsz);

    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL(1, write_buffer_call_count);
    TEST_ASSERT_EQUAL_size_t(fsz, last_write_buffer_len);
    TEST_ASSERT_EQUAL_HEX8(0x60, last_write_buffer_data[0]);
    TEST_ASSERT_EQUAL_HEX8(CMD_DATA,
        atomic_load_explicit(&bcast_reply_cmd, memory_order_relaxed));
}

/* CMD_DATA, short frame: zero-padded to frame_size */
void test_bcast_rx_cmd_data_short_padded(void)
{
    const size_t fsz = 10;
    broadcast_frame_size_cfg = fsz;

    uint8_t frame[MAX_PAYLOAD];
    memset(frame, 0, sizeof(frame));
    frame[0] = 0x60;
    memset(frame + 1, 0xBB, 4); /* 5 bytes total */

    bool ok = bcast_process_decoded_frame(frame, 5, CMD_DATA, fsz);

    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL(1, write_buffer_call_count);
    TEST_ASSERT_EQUAL_size_t(fsz, last_write_buffer_len);
    /* Tail must be zero-filled */
    for (size_t i = 5; i < fsz; i++)
        TEST_ASSERT_EQUAL_HEX8(0x00, last_write_buffer_data[i]);
    TEST_ASSERT_EQUAL_HEX8(CMD_DATA,
        atomic_load_explicit(&bcast_reply_cmd, memory_order_relaxed));
}

/* CMD_DATA, oversized: discarded, write_buffer never called */
void test_bcast_rx_cmd_data_oversized_discarded(void)
{
    const size_t fsz = 10;
    broadcast_frame_size_cfg = fsz;

    uint8_t frame[MAX_PAYLOAD];
    memset(frame, 0x11, sizeof(frame));

    bool ok = bcast_process_decoded_frame(frame, (int)fsz + 5, CMD_DATA, fsz);

    TEST_ASSERT_FALSE(ok);
    TEST_ASSERT_EQUAL(0, write_buffer_call_count);
    /* bcast_reply_cmd is still set even for discarded frames */
    TEST_ASSERT_EQUAL_HEX8(CMD_DATA,
        atomic_load_explicit(&bcast_reply_cmd, memory_order_relaxed));
}

/* CMD_AX25CALLSIGN, payload fits: header + length prefix injected, payload
 * shifted, zero-padded */
void test_bcast_rx_vara_header_injected(void)
{
    const size_t fsz = 10;
    broadcast_frame_size_cfg = fsz;

    uint8_t frame[MAX_PAYLOAD];
    memset(frame, 0, sizeof(frame));
    /* 5-byte payload: 0x01 0x02 0x03 0x04 0x05 */
    for (int i = 0; i < 5; i++) frame[i] = (uint8_t)(i + 1);

    bool ok = bcast_process_decoded_frame(frame, 5, CMD_AX25CALLSIGN, fsz);

    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL(1, write_buffer_call_count);
    TEST_ASSERT_EQUAL_size_t(fsz, last_write_buffer_len);
    /* frame[0] is the broadcast header byte with the length-prefix flag set */
    TEST_ASSERT_EQUAL_HEX8(BCAST_HDR_BYTE_LEN, last_write_buffer_data[0]);
    /* 2-byte big-endian length prefix = 5 */
    TEST_ASSERT_EQUAL_HEX8(0x00, last_write_buffer_data[1]);
    TEST_ASSERT_EQUAL_HEX8(0x05, last_write_buffer_data[2]);
    /* Original payload shifted to [3..7] */
    for (int i = 0; i < 5; i++)
        TEST_ASSERT_EQUAL_HEX8((uint8_t)(i + 1),
            last_write_buffer_data[HEADER_SIZE + BCAST_LEN_SIZE + i]);
    /* Tail bytes [8..9] must be zero */
    for (size_t i = 8; i < fsz; i++)
        TEST_ASSERT_EQUAL_HEX8(0x00, last_write_buffer_data[i]);
    /* Reply cmd normalised to CMD_AX25CALLSIGN regardless of CMD_AX25 vs _CALLSIGN */
    TEST_ASSERT_EQUAL_HEX8(CMD_AX25CALLSIGN,
        atomic_load_explicit(&bcast_reply_cmd, memory_order_relaxed));
}

/* CMD_AX25 (bare): reply cmd normalised to CMD_AX25CALLSIGN */
void test_bcast_rx_cmd_ax25_reply_cmd(void)
{
    const size_t fsz = 10;
    broadcast_frame_size_cfg = fsz;

    uint8_t frame[MAX_PAYLOAD];
    memset(frame, 0xCC, 3);

    bool ok = bcast_process_decoded_frame(frame, 3, CMD_AX25, fsz);

    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_HEX8(CMD_AX25CALLSIGN,
        atomic_load_explicit(&bcast_reply_cmd, memory_order_relaxed));
}

/* CMD_AX25CALLSIGN, payload longer than frame_size-(header+len): truncated */
void test_bcast_rx_vara_long_payload_truncated(void)
{
    const size_t fsz = 10;
    broadcast_frame_size_cfg = fsz;
    /* max_payload = fsz - HEADER_SIZE - BCAST_LEN_SIZE = 7; send 12 bytes */
    const int raw_len = 12;
    const int max_payload = (int)fsz - HEADER_SIZE - BCAST_LEN_SIZE; /* 7 */

    uint8_t frame[MAX_PAYLOAD];
    for (int i = 0; i < raw_len; i++) frame[i] = (uint8_t)(0x10 + i);

    bool ok = bcast_process_decoded_frame(frame, raw_len, CMD_AX25CALLSIGN, fsz);

    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL(1, write_buffer_call_count);
    TEST_ASSERT_EQUAL_size_t(fsz, last_write_buffer_len);
    TEST_ASSERT_EQUAL_HEX8(BCAST_HDR_BYTE_LEN, last_write_buffer_data[0]);
    /* Length prefix reflects the truncated length (7) */
    TEST_ASSERT_EQUAL_HEX8(0x00, last_write_buffer_data[1]);
    TEST_ASSERT_EQUAL_HEX8((uint8_t)max_payload, last_write_buffer_data[2]);
    /* Only the first 7 bytes of the original payload must survive */
    for (int i = 0; i < max_payload; i++)
        TEST_ASSERT_EQUAL_HEX8((uint8_t)(0x10 + i),
            last_write_buffer_data[HEADER_SIZE + BCAST_LEN_SIZE + i]);
}

/* bcast_get_tx_payload: CMD_DATA → full frame, payload_len == frame_size */
void test_bcast_tx_cmd_data_full_frame(void)
{
    const size_t fsz = 10;
    uint8_t frame[10];
    memset(frame, 0xDD, fsz);
    frame[0] = 0x60; /* Mercury header present */

    atomic_store_explicit(&bcast_reply_cmd, CMD_DATA, memory_order_relaxed);

    uint8_t *payload = NULL;
    int plen = 0;
    uint8_t cmd = bcast_get_tx_payload(frame, fsz, &payload, &plen);

    TEST_ASSERT_EQUAL_HEX8(CMD_DATA, cmd);
    TEST_ASSERT_EQUAL_PTR(frame, payload);       /* must point to start of frame */
    TEST_ASSERT_EQUAL_INT((int)fsz, plen);        /* full frame_size */
}

/* bcast_get_tx_payload: CMD_AX25CALLSIGN → header stripped, payload_len == frame_size-1 */
void test_bcast_tx_vara_strips_header(void)
{
    const size_t fsz = 10;
    uint8_t frame[10];
    frame[0] = BCAST_HDR_BYTE;
    memset(frame + 1, 0xEE, fsz - 1);

    atomic_store_explicit(&bcast_reply_cmd, CMD_AX25CALLSIGN, memory_order_relaxed);

    uint8_t *payload = NULL;
    int plen = 0;
    uint8_t cmd = bcast_get_tx_payload(frame, fsz, &payload, &plen);

    TEST_ASSERT_EQUAL_HEX8(CMD_AX25CALLSIGN, cmd);
    TEST_ASSERT_EQUAL_PTR(frame + HEADER_SIZE, payload); /* skips the Mercury header */
    TEST_ASSERT_EQUAL_INT((int)fsz - HEADER_SIZE, plen); /* one byte shorter */
}

/* Round-trip: a VARA frame run through TX framing then RX extraction must yield
 * exactly the original payload (length + bytes), with the modem zero padding
 * stripped. This is the core of the length-prefix fix. */
void test_bcast_vara_length_roundtrip(void)
{
    const size_t fsz = 32;
    broadcast_frame_size_cfg = fsz;

    const int orig_len = 11;
    uint8_t orig[32];
    for (int i = 0; i < orig_len; i++) orig[i] = (uint8_t)(0xA0 + i);

    /* TX: build the on-air frame (captured by the mock write_buffer). */
    uint8_t txframe[MAX_PAYLOAD];
    memset(txframe, 0, sizeof(txframe));
    memcpy(txframe, orig, orig_len);
    bool ok = bcast_process_decoded_frame(txframe, orig_len, CMD_AX25CALLSIGN, fsz);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_size_t(fsz, last_write_buffer_len);

    /* RX: extract from the framed buffer; reply_cmd was latched by the TX call. */
    uint8_t rxframe[MAX_PAYLOAD];
    memcpy(rxframe, last_write_buffer_data, fsz);
    uint8_t *payload = NULL;
    int plen = 0;
    uint8_t cmd = bcast_get_tx_payload(rxframe, fsz, &payload, &plen);

    TEST_ASSERT_EQUAL_HEX8(CMD_AX25CALLSIGN, cmd);
    TEST_ASSERT_EQUAL_INT(orig_len, plen);              /* exact original length */
    TEST_ASSERT_EQUAL_PTR(rxframe + HEADER_SIZE + BCAST_LEN_SIZE, payload);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(orig, payload, orig_len); /* exact original bytes */
}

/* Receive-only station: a length-prefixed frame must be delivered as the exact
 * AX.25 payload with CMD_AX25CALLSIGN even when bcast_reply_cmd is still the
 * default CMD_DATA (local client has not transmitted). Regression for the bug
 * where receive-only stations forwarded the raw padded frame as CMD_DATA. */
void test_bcast_tx_lenprefix_ignores_reply_cmd_default(void)
{
    const size_t fsz = 32;
    const int len = 9;

    uint8_t frame[32];
    memset(frame, 0, sizeof(frame));
    frame[0] = BCAST_HDR_BYTE_LEN;                 /* BROADCAST_DATA + len flag */
    frame[1] = (uint8_t)((len >> 8) & 0xFF);
    frame[2] = (uint8_t)(len & 0xFF);
    for (int i = 0; i < len; i++)
        frame[HEADER_SIZE + BCAST_LEN_SIZE + i] = (uint8_t)(0x40 + i);

    /* Default / receive-only state */
    atomic_store_explicit(&bcast_reply_cmd, CMD_DATA, memory_order_relaxed);

    uint8_t *payload = NULL;
    int plen = 0;
    uint8_t cmd = bcast_get_tx_payload(frame, fsz, &payload, &plen);

    TEST_ASSERT_EQUAL_HEX8(CMD_AX25CALLSIGN, cmd);     /* NOT CMD_DATA */
    TEST_ASSERT_EQUAL_INT(len, plen);                  /* exact length, not fsz */
    TEST_ASSERT_EQUAL_PTR(frame + HEADER_SIZE + BCAST_LEN_SIZE, payload);
    for (int i = 0; i < len; i++)
        TEST_ASSERT_EQUAL_HEX8((uint8_t)(0x40 + i), payload[i]);
}

/* Standard-KISS roundtrip: a frame the client transmitted with KISS cmd 0x00
 * (CMD_AX25, e.g. Reticulum) must carry the BCAST_EXT_KISS_STD header bit on
 * the air and be delivered on the far side with cmd 0x00 — even on a
 * receive-only station (reply_cmd still at its default). VarAC-framed frames
 * (cmd 0x01) must NOT carry the bit and keep being delivered as 0x01, which
 * test_bcast_vara_length_roundtrip guards. */
void test_bcast_std_kiss_roundtrip(void)
{
    const size_t fsz = 32;
    broadcast_frame_size_cfg = fsz;

    const int orig_len = 13;
    uint8_t orig[32];
    for (int i = 0; i < orig_len; i++) orig[i] = (uint8_t)(0x90 + i);

    /* TX: client framed with standard KISS cmd 0x00 */
    uint8_t txframe[MAX_PAYLOAD];
    memset(txframe, 0, sizeof(txframe));
    memcpy(txframe, orig, orig_len);
    bool ok = bcast_process_decoded_frame(txframe, orig_len, CMD_AX25, fsz);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_size_t(fsz, last_write_buffer_len);
    /* On-air header carries len-prefix + std-KISS bits */
    TEST_ASSERT_EQUAL_HEX8(BCAST_HDR_BYTE_STD, last_write_buffer_data[0]);

    /* RX on a receive-only station: reply_cmd still at its connect default */
    uint8_t rxframe[MAX_PAYLOAD];
    memcpy(rxframe, last_write_buffer_data, fsz);
    atomic_store_explicit(&bcast_reply_cmd, CMD_DATA, memory_order_relaxed);

    uint8_t *payload = NULL;
    int plen = 0;
    uint8_t cmd = bcast_get_tx_payload(rxframe, fsz, &payload, &plen);

    TEST_ASSERT_EQUAL_HEX8(CMD_AX25, cmd);              /* 0x00, not 0x01 */
    TEST_ASSERT_EQUAL_INT(orig_len, plen);
    TEST_ASSERT_EQUAL_PTR(rxframe + HEADER_SIZE + BCAST_LEN_SIZE, payload);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(orig, payload, orig_len);
}

int main(void)
{
    UNITY_BEGIN();
    /* Command parser tests */
    RUN_TEST(test_cmd_mycall);
    RUN_TEST(test_cmd_listen_on);
    RUN_TEST(test_cmd_listen_off);
    RUN_TEST(test_cmd_public_on);
    RUN_TEST(test_cmd_compression);
    RUN_TEST(test_cmd_chat_on);
    RUN_TEST(test_cmd_bw500);
    RUN_TEST(test_cmd_bw2300);
    RUN_TEST(test_cmd_bw_invalid);
    RUN_TEST(test_cmd_retries);
    RUN_TEST(test_cmd_connect);
    RUN_TEST(test_cmd_disconnect);
    RUN_TEST(test_cmd_cqframe);
    RUN_TEST(test_cmd_p2p);
    RUN_TEST(test_cmd_unknown);
    RUN_TEST(test_cmd_submit_failure);
    RUN_TEST(test_cmd_abort);
    RUN_TEST(test_cmd_abort_submit_failure);
    RUN_TEST(test_cmd_version);
    RUN_TEST(test_cmd_ignorekissdcd);
    RUN_TEST(test_cmd_listen_cq);
    /* CALLINT command tests */
    RUN_TEST(test_cmd_callint_valid);
    RUN_TEST(test_cmd_callint_zero);
    RUN_TEST(test_cmd_callint_no_arg);
    RUN_TEST(test_cmd_callint_nonnumeric);
    RUN_TEST(test_cmd_callint_negative);
    RUN_TEST(test_process_control_bytes_multiline);
    /* Status emitter tests */
    RUN_TEST(test_tnc_send_disconnected);
    RUN_TEST(test_tnc_send_pending);
    RUN_TEST(test_tnc_send_cancelpending);
    RUN_TEST(test_tnc_send_buffer);
    RUN_TEST(test_tnc_send_sn);
    RUN_TEST(test_tnc_send_bitrate);
    RUN_TEST(test_tnc_send_connected);
    RUN_TEST(test_tnc_send_cqframe);
    /* Broadcast framing helper tests */
    RUN_TEST(test_bcast_rx_cmd_data_exact_size);
    RUN_TEST(test_bcast_rx_cmd_data_short_padded);
    RUN_TEST(test_bcast_rx_cmd_data_oversized_discarded);
    RUN_TEST(test_bcast_rx_vara_header_injected);
    RUN_TEST(test_bcast_rx_cmd_ax25_reply_cmd);
    RUN_TEST(test_bcast_rx_vara_long_payload_truncated);
    RUN_TEST(test_bcast_tx_cmd_data_full_frame);
    RUN_TEST(test_bcast_tx_vara_strips_header);
    RUN_TEST(test_bcast_vara_length_roundtrip);
    RUN_TEST(test_bcast_tx_lenprefix_ignores_reply_cmd_default);
    RUN_TEST(test_bcast_std_kiss_roundtrip);
    return UNITY_END();
}
