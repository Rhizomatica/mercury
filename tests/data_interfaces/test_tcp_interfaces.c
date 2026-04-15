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
#include "fsm.h"
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
int write_buffer(cbuf_handle_t cbuf, uint8_t *data, size_t len) { (void)cbuf; (void)data; (void)len; return 0; }
void clear_buffer(cbuf_handle_t cbuf) { (void)cbuf; }

/* ---- kiss stubs ---- */

int kiss_write_frame(uint8_t *a, int b, uint8_t *c) { (void)a; (void)b; (void)c; return 0; }
int kiss_read(uint8_t b, uint8_t *c) { (void)b; (void)c; return 0; }
void kiss_reset_state(void) { }

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
    return UNITY_END();
}
