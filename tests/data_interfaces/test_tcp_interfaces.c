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

void arq_set_trx(int trx) { arq_conn.TRX = trx; }
int  arq_get_trx(void)    { return arq_conn.TRX; }

void arq_conn_get_calls(char *my_call, char *src_addr, char *dst_addr, size_t bufsz)
{
    if (bufsz == 0) return;
    if (my_call)  snprintf(my_call,  bufsz, "%s", arq_conn.my_call_sign);
    if (src_addr) snprintf(src_addr, bufsz, "%s", arq_conn.src_addr);
    if (dst_addr) snprintf(dst_addr, bufsz, "%s", arq_conn.dst_addr);
}

/* ---- message_store stubs ---- */

static size_t mock_msg_store_count = 0;
static char   mock_msg_lines[4][128];

void msg_store_feed(const char *plane, const char *dir, const char *peer,
                    const uint8_t *data, size_t len)
{
    (void)plane; (void)dir; (void)peer; (void)data; (void)len;
}

size_t msg_store_count(void)
{
    return mock_msg_store_count;
}

size_t msg_store_get(size_t index, char *buf, size_t buf_cap)
{
    if (index >= mock_msg_store_count || !buf || buf_cap == 0)
        return 0;
    size_t n = strlen(mock_msg_lines[index]);
    if (n >= buf_cap)
        n = buf_cap - 1;
    memcpy(buf, mock_msg_lines[index], n);
    buf[n] = '\0';
    return n;
}

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
ssize_t tcp_read(int pt, uint8_t *b, size_t s) { (void)pt; (void)b; (void)s; return 0; }
int tcp_close(int pt) { (void)pt; return 0; }

/* ---- radio_io stubs ---- */

bool radio_io_enabled(void) { return false; }
int radio_io_key_on(void) { return 0; }
int radio_io_key_off(void) { return 0; }

/* ---- modem tuning-carrier stubs ---- */

static float mock_tune_start_dbfs  = 0.0f;
static int   mock_tune_start_calls = 0;
static int   mock_tune_start_rc    = 0;     /* what modem_tune_start returns */
static int   mock_tune_stop_calls  = 0;
static float mock_tune_level_dbfs  = -15.0f;

int modem_tune_start(float dbfs)
{
    mock_tune_start_dbfs = dbfs;
    mock_tune_start_calls++;
    return mock_tune_start_rc;
}
void  modem_tune_stop(void)        { mock_tune_stop_calls++; }
bool  modem_tune_active(void)      { return false; }
float modem_tune_level_dbfs(void)  { return mock_tune_level_dbfs; }

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
/* How many tcp_write() calls had already happened when a line was queued.
 * Lets a test pin the ORDER of a direct write against a queued notification. */
static int queued_after_tcp_writes = -1;
/* Full sequence, not just the last line: MYCALL acknowledges every callsign
 * it accepted, so a test has to see all of them and in order. */
#define MAX_QUEUED_LINES 8
static char queued_lines[MAX_QUEUED_LINES][256];
static int  queued_line_count = 0;

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
        queued_after_tcp_writes = tcp_write_call_count;
        if (queued_line_count < MAX_QUEUED_LINES && len < sizeof(queued_lines[0]))
        {
            memset(queued_lines[queued_line_count], 0, sizeof(queued_lines[0]));
            memcpy(queued_lines[queued_line_count], data_ptr, len);
            queued_line_count++;
        }
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

/* Matches the _Atomic definition in common/mercury_engine.c; a plain
 * volatile here is a conflicting declaration of the same object. */
_Atomic bool shutdown_ = false;
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
    queued_after_tcp_writes = -1;
    memset(queued_lines, 0, sizeof(queued_lines));
    queued_line_count = 0;
    memset(&arq_conn, 0, sizeof(arq_conn));
    mock_bandwidth_hz = 2300;
    mock_msg_store_count = 0;
    memset(mock_msg_lines, 0, sizeof(mock_msg_lines));

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

    /* Tuning carrier */
    mock_tune_start_dbfs  = 0.0f;
    mock_tune_start_calls = 0;
    mock_tune_start_rc    = 0;
    mock_tune_stop_calls  = 0;
    mock_tune_level_dbfs  = -15.0f;
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

/* REGISTERED answers MYCALL, so the host must not see it before the OK.
 * OK is a direct tcp_write() while notifications are queued for another
 * thread, so emitting REGISTERED from the ARQ event loop could put it on the
 * wire first -- secondary callsigns widen that window, hence the extra tokens
 * here.  Pin the order, not just the content. */
void test_cmd_mycall_registered_follows_ok(void)
{
    char cmd[] = "MYCALL TEST1 SEC1 SEC2";
    execute_control_command(cmd);

    assert_ok_response();
    /* First line out is the primary; the secondaries follow it. */
    TEST_ASSERT_EQUAL_STRING("REGISTERED TEST1\r", queued_lines[0]);
    TEST_ASSERT_GREATER_THAN_INT(0, queued_after_tcp_writes);
}

/* "REGISTERED <Call>" is per call sign in VARA, and MYCALL can carry
 * secondaries that Mercury will answer for, so each accepted one is
 * acknowledged -- in the order given, primary first. */
void test_cmd_mycall_registers_every_callsign(void)
{
    char cmd[] = "MYCALL TEST1 SEC1 SEC2";
    execute_control_command(cmd);

    assert_ok_response();
    TEST_ASSERT_EQUAL_INT(3, queued_line_count);
    TEST_ASSERT_EQUAL_STRING("REGISTERED TEST1\r", queued_lines[0]);
    TEST_ASSERT_EQUAL_STRING("REGISTERED SEC1\r",  queued_lines[1]);
    TEST_ASSERT_EQUAL_STRING("REGISTERED SEC2\r",  queued_lines[2]);
}

/* Past CALLSIGN_MAX_SECONDARY the ARQ layer drops the extras, so the host
 * must not be told they are registered -- it would address a callsign this
 * station never answers. */
void test_cmd_mycall_does_not_register_dropped_secondaries(void)
{
    char cmd[] = "MYCALL TEST1 S1 S2 S3 S4 S5 S6";
    execute_control_command(cmd);

    assert_ok_response();
    TEST_ASSERT_EQUAL_INT(1 + CALLSIGN_MAX_SECONDARY, queued_line_count);
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

/* ---- TUNE: the ATU tuning carrier (VARA syntax) ----
 * This command keys the transmitter with no protocol underneath it, so the
 * parser boundary is safety-relevant: a level must reach the modem verbatim,
 * a refusal must NOT be reported as success, and OFF must always stop. */

void test_cmd_tune_on(void)
{
    char cmd[] = "TUNE -15";
    execute_control_command(cmd);

    assert_ok_response();
    TEST_ASSERT_EQUAL_INT(1, mock_tune_start_calls);
    TEST_ASSERT_EQUAL_FLOAT(-15.0f, mock_tune_start_dbfs);
}

void test_cmd_tune_on_fractional_level(void)
{
    char cmd[] = "TUNE -12.5";
    execute_control_command(cmd);

    assert_ok_response();
    TEST_ASSERT_EQUAL_FLOAT(-12.5f, mock_tune_start_dbfs);
}

void test_cmd_tune_off(void)
{
    char cmd[] = "TUNE OFF";
    execute_control_command(cmd);

    assert_ok_response();
    TEST_ASSERT_EQUAL_INT(1, mock_tune_stop_calls);
    TEST_ASSERT_EQUAL_INT(0, mock_tune_start_calls);
}

void test_cmd_tune_query_reports_level(void)
{
    mock_tune_level_dbfs = -15.0f;
    char cmd[] = "TUNE ?";
    execute_control_command(cmd);

    TEST_ASSERT_EQUAL(1, tcp_write_call_count);
    TEST_ASSERT_EQUAL_STRING("TUNE -15\r", (char *)last_tcp_write_buf);
    /* A query must not key anything. */
    TEST_ASSERT_EQUAL_INT(0, mock_tune_start_calls);
    TEST_ASSERT_EQUAL_INT(0, mock_tune_stop_calls);
}

void test_cmd_tune_refusal_is_not_ok(void)
{
    /* modem_tune_start rejects out-of-range levels and refuses while a link
     * is up; the host must see WRONG, never OK. */
    mock_tune_start_rc = -2;
    char cmd[] = "TUNE -15";
    execute_control_command(cmd);

    assert_wrong_response();
    TEST_ASSERT_EQUAL_INT(1, mock_tune_start_calls);
}

void test_cmd_tune_missing_argument(void)
{
    char cmd[] = "TUNE";
    execute_control_command(cmd);

    assert_wrong_response();
    TEST_ASSERT_EQUAL_INT(0, mock_tune_start_calls);
}

void test_cmd_tune_garbage_argument(void)
{
    char cmd[] = "TUNE loud";
    execute_control_command(cmd);

    assert_wrong_response();
    TEST_ASSERT_EQUAL_INT(0, mock_tune_start_calls);
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

void test_tnc_send_registered(void)
{
    tnc_send_registered("TESTA");
    TEST_ASSERT_EQUAL_STRING("REGISTERED TESTA\r", last_queued_line);
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
#define BCAST_EXT_KISS_DATA  0x04
#define BCAST_HDR_BYTE_LEN   (BCAST_HDR_BYTE | BCAST_EXT_LEN_PREFIX)
#define BCAST_HDR_BYTE_STD   (BCAST_HDR_BYTE | BCAST_EXT_LEN_PREFIX | BCAST_EXT_KISS_STD)
#define BCAST_HDR_BYTE_DATA  (BCAST_HDR_BYTE | BCAST_EXT_LEN_PREFIX | BCAST_EXT_KISS_DATA)
/* A RAW hermes-broadcast header: broadcast packet type with a ZERO extension,
 * which is what hermes_write_frame_header(..., PACKET_RQ_CONFIG, 0) produces.
 * Distinct from BCAST_HDR_BYTE_DATA above, which is the header Mercury writes
 * when it WRAPS a client payload. */
#define BCAST_HDR_RAW_CONTROL \
    ((uint8_t)(PACKET_TYPE_BROADCAST_CONTROL << PACKET_TYPE_SHIFT))

/* CMD_DATA at exact frame_size is a MESSAGE and is framed like any other.
 *
 * This used to be passed through unchanged, inferred from the payload's first
 * byte.  A sender that means "this is a modem frame" now says so with
 * CMD_MODEM_FRAME; CMD_DATA no longer carries a second meaning. */
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
    /* Mercury's own header now leads the frame, not the payload's first byte. */
    TEST_ASSERT_EQUAL_HEX8(PACKET_TYPE_BROADCAST_DATA,
                           frame_header_packet_type(last_write_buffer_data[0]));
    TEST_ASSERT_TRUE(frame_header_extension(last_write_buffer_data[0]) & BCAST_EXT_LEN_PREFIX);
}

/* CMD_DATA, short frame with a broadcast-header-looking first byte (0x60): NOT
 * hermes-broadcast, which always fills frame_size, so it must be wrapped like
 * an unformatted VARA frame rather than passed through raw. */
/* 0x60 is the one genuinely ambiguous first byte: it is
 * PACKET_TYPE_BROADCAST_CONTROL with a zero extension, i.e. byte-for-byte what
 * hermes-broadcast puts on a config frame -- and also a printable backtick that
 * a beacon could in principle start with.  Nothing in the frame distinguishes
 * them.
 *
 * That ambiguity no longer exists.  A sender that means "this is a modem
 * frame" says CMD_MODEM_FRAME; CMD_DATA always means a message.  So 0x60 --
 * or any other first byte -- gets no special treatment, and a beacon that
 * happens to start with a backtick is framed like every other message. */
void test_bcast_rx_cmd_data_0x60_is_not_special(void)
{
    const size_t fsz = 10;
    broadcast_frame_size_cfg = fsz;

    uint8_t frame[MAX_PAYLOAD];
    memset(frame, 0, sizeof(frame));
    frame[0] = 0x60;                 /* BROADCAST_CONTROL, extension 0 */
    memset(frame + 1, 0xBB, 4);      /* 5 bytes total, short */

    bool ok = bcast_process_decoded_frame(frame, 5, CMD_DATA, fsz);

    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL(1, write_buffer_call_count);
    TEST_ASSERT_EQUAL_size_t(fsz, last_write_buffer_len);

    /* Framed: Mercury's header, then the true length, then the payload. */
    TEST_ASSERT_EQUAL_HEX8(PACKET_TYPE_BROADCAST_DATA,
                           frame_header_packet_type(last_write_buffer_data[0]));
    TEST_ASSERT_TRUE(frame_header_extension(last_write_buffer_data[0]) & BCAST_EXT_LEN_PREFIX);

    uint8_t *payload = NULL;
    int plen = 0;
    uint8_t cmd = bcast_get_tx_payload(last_write_buffer_data, fsz, &payload, &plen);
    TEST_ASSERT_EQUAL_HEX8(CMD_DATA, cmd);
    TEST_ASSERT_EQUAL_INT(5, plen);              /* exact original length */
    TEST_ASSERT_EQUAL_HEX8(0x60, payload[0]);
    for (int i = 1; i < 5; i++)
        TEST_ASSERT_EQUAL_HEX8(0xBB, payload[i]);
}

/* A short beacon whose body begins with a lowercase letter ('a'=0x61, 'z'=0x7A)
 * decodes as PACKET_TYPE_BROADCAST_CONTROL (0x60-0x7F first-byte range).  It
 * must still be wrapped — the length check, not just the header bits, is what
 * keeps it from being mistaken for a full hermes-broadcast frame. */
void test_bcast_rx_cmd_data_short_lowercase_wrapped(void)
{
    const size_t fsz = 126;
    broadcast_frame_size_cfg = fsz;

    const uint8_t firsts[] = { 'a', 'z' };  /* 0x61, 0x7A → packet type 3 */
    for (size_t k = 0; k < sizeof(firsts) / sizeof(firsts[0]); k++)
    {
        uint8_t frame[MAX_PAYLOAD];
        memset(frame, 0, sizeof(frame));
        frame[0] = firsts[k];
        memset(frame + 1, 0xCC, 19);        /* 20 bytes total, like a beacon */

        write_buffer_call_count = 0;
        memset(last_write_buffer_data, 0, sizeof(last_write_buffer_data));

        bool ok = bcast_process_decoded_frame(frame, 20, CMD_DATA, fsz);

        TEST_ASSERT_TRUE(ok);
        TEST_ASSERT_EQUAL(1, write_buffer_call_count);
        TEST_ASSERT_EQUAL_size_t(fsz, last_write_buffer_len);
        TEST_ASSERT_EQUAL_HEX8(BCAST_HDR_BYTE_DATA, last_write_buffer_data[0]);
        TEST_ASSERT_EQUAL_HEX8(0x00, last_write_buffer_data[1]);
        TEST_ASSERT_EQUAL_HEX8(0x14, last_write_buffer_data[2]); /* len = 20 */
        TEST_ASSERT_EQUAL_HEX8(firsts[k], last_write_buffer_data[3]);
        TEST_ASSERT_EQUAL_HEX8(0xCC, last_write_buffer_data[4]);
    }
}

/* CMD_DATA, oversized, with a real Mercury broadcast header (hermes-broadcast):
 * discarded, write_buffer never called */
void test_bcast_rx_cmd_data_oversized_discarded(void)
{
    const size_t fsz = 10;
    broadcast_frame_size_cfg = fsz;

    uint8_t frame[MAX_PAYLOAD];
    memset(frame, 0x11, sizeof(frame));
    frame[0] = BCAST_HDR_BYTE; /* PACKET_TYPE_BROADCAST_DATA header already in place */

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

/* A modem frame declared as such must reach the air byte for byte.
 *
 * This is the regression that mattered: the UI's file transfer sent its frames
 * through the chat path (CMD_AX25), which wraps unconditionally, so every
 * 1180-byte RaptorQ frame was truncated to 1177 to make room for a header it
 * did not need.  The far side then had 1177 bytes of a 1180-byte frame and
 * discarded all of them.  CMD_MODEM_FRAME says what the payload is instead of
 * leaving Mercury to infer it. */
void test_bcast_tx_modem_frame_command_passes_through_untouched(void)
{
    const size_t fsz = 32;
    uint8_t orig[32];
    /* Deliberately a first byte that does NOT look like a broadcast type, so
     * this can only pass by the command being honoured, not by the heuristic. */
    for (size_t i = 0; i < fsz; i++) orig[i] = (uint8_t)(0x11 + i);

    uint8_t txframe[MAX_PAYLOAD];
    memset(txframe, 0, sizeof(txframe));
    memcpy(txframe, orig, fsz);

    bool ok = bcast_process_decoded_frame(txframe, (int)fsz, CMD_MODEM_FRAME, fsz);
    TEST_ASSERT_TRUE(ok);

    /* Not truncated, nothing injected. */
    TEST_ASSERT_EQUAL_size_t(fsz, last_write_buffer_len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(orig, last_write_buffer_data, fsz);
}

/* And the same payload sent the way chat is sent IS framed -- which is what
 * silently broke the file transfer, so pin the difference. */
void test_bcast_tx_same_frame_as_chat_is_truncated(void)
{
    const size_t fsz = 32;
    uint8_t orig[32];
    for (size_t i = 0; i < fsz; i++) orig[i] = (uint8_t)(0x11 + i);

    uint8_t txframe[MAX_PAYLOAD];
    memset(txframe, 0, sizeof(txframe));
    memcpy(txframe, orig, fsz);

    TEST_ASSERT_TRUE(bcast_process_decoded_frame(txframe, (int)fsz, CMD_AX25, fsz));

    uint8_t rxframe[MAX_PAYLOAD];
    memcpy(rxframe, last_write_buffer_data, fsz);
    uint8_t *payload = NULL;
    int plen = 0;
    bcast_get_tx_payload(rxframe, fsz, &payload, &plen);

    /* 3 bytes short: the header and length prefix took their room. */
    TEST_ASSERT_EQUAL_INT((int)(fsz - HEADER_SIZE - BCAST_LEN_SIZE), plen);
}

/* What happens to a broadcast message that is EXACTLY one frame long?
 *
 * The raw-vs-wrap heuristic keys on the first byte: bits 7:5 == 3 or 4 look
 * like a broadcast packet type, and 0x60..0x7F is backtick plus every lowercase
 * letter.  So "hello ..." padded to the frame size presents a broadcast-looking
 * header.  These pin what each kind of sender actually gets, because the answer
 * differs and the difference is the whole safety argument. */

/* Chat (CMD_AX25) is ALWAYS wrapped, whatever it contains: needs_wrap is true
 * for CMD_AX25 unconditionally, so the heuristic never applies to it.  A
 * full-length chat line survives intact with its exact length. */
void test_bcast_tx_full_size_chat_is_still_wrapped(void)
{
    const size_t fsz = 32;
    uint8_t orig[32];
    memset(orig, 'x', sizeof(orig));
    orig[0] = 'h';                     /* 0x68 -> looks like packet type 3 */

    uint8_t txframe[MAX_PAYLOAD];
    memset(txframe, 0, sizeof(txframe));
    memcpy(txframe, orig, fsz);

    /* Exactly one frame of chat.  It cannot fit a header+len prefix as well, so
     * the wrap path truncates to make room -- lossy, but it is delivered as a
     * message rather than misread as a modem frame. */
    bool ok = bcast_process_decoded_frame(txframe, (int)fsz, CMD_AX25, fsz);
    TEST_ASSERT_TRUE(ok);

    uint8_t rxframe[MAX_PAYLOAD];
    memcpy(rxframe, last_write_buffer_data, fsz);
    uint8_t *payload = NULL;
    int plen = 0;
    uint8_t cmd = bcast_get_tx_payload(rxframe, fsz, &payload, &plen);

    /* Framed and delivered as chat, not passed through as a raw modem frame. */
    TEST_ASSERT_EQUAL_HEX8(CMD_AX25, cmd);
    TEST_ASSERT_EQUAL_INT((int)(fsz - HEADER_SIZE - BCAST_LEN_SIZE), plen);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(orig, payload, plen);
}

/* A full-size CMD_DATA payload is a MESSAGE and is framed -- so it is truncated
 * by 3 bytes to make room, exactly as chat is.  A sender with a real modem
 * frame must say CMD_MODEM_FRAME; nothing about the payload's contents changes
 * the answer any more. */
void test_bcast_tx_full_size_cmd_data_is_framed(void)
{
    const size_t fsz = 32;
    uint8_t orig[32];
    for (size_t i = 0; i < fsz; i++) orig[i] = (uint8_t)(0x40 + i);
    orig[0] = 'h';                     /* 0x68 -> packet type 3, ext 8 */

    uint8_t txframe[MAX_PAYLOAD];
    memset(txframe, 0, sizeof(txframe));
    memcpy(txframe, orig, fsz);

    TEST_ASSERT_TRUE(bcast_process_decoded_frame(txframe, (int)fsz, CMD_DATA, fsz));

    uint8_t rxframe[MAX_PAYLOAD];
    memcpy(rxframe, last_write_buffer_data, fsz);
    uint8_t *payload = NULL;
    int plen = 0;
    uint8_t cmd = bcast_get_tx_payload(rxframe, fsz, &payload, &plen);

    TEST_ASSERT_EQUAL_HEX8(CMD_DATA, cmd);
    TEST_ASSERT_EQUAL_INT((int)(fsz - HEADER_SIZE - BCAST_LEN_SIZE), plen);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(orig, payload, plen);
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
 * receive-only station (reply_cmd still at its default). VARA-framed frames
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

/* CMD_DATA with no Mercury header (VARA beacon/ping): the frame must be
 * wrapped with the broadcast header + length prefix, flagged with the
 * BCAST_EXT_KISS_DATA bit so the far side mirrors the 0x02 framing.  This is
 * the regression for the bug where a VARA beacon was queued raw (its first
 * byte collided with an ARQ packet type) and dropped on the receiver. */
void test_bcast_rx_cmd_data_vara_beacon_wrapped(void)
{
    const size_t fsz = 10;
    broadcast_frame_size_cfg = fsz;

    uint8_t frame[MAX_PAYLOAD];
    memset(frame, 0, sizeof(frame));
    /* VARA beacon body: first byte 0x20 decodes as an ARQ type, i.e. NOT a
     * broadcast header, so it must be treated as an unformatted client frame. */
    for (int i = 0; i < 5; i++) frame[i] = (uint8_t)(0x20 + i);

    bool ok = bcast_process_decoded_frame(frame, 5, CMD_DATA, fsz);

    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL(1, write_buffer_call_count);
    TEST_ASSERT_EQUAL_size_t(fsz, last_write_buffer_len);
    TEST_ASSERT_EQUAL_HEX8(BCAST_HDR_BYTE_DATA, last_write_buffer_data[0]);
    TEST_ASSERT_EQUAL_HEX8(0x00, last_write_buffer_data[1]);
    TEST_ASSERT_EQUAL_HEX8(0x05, last_write_buffer_data[2]);
    for (int i = 0; i < 5; i++)
        TEST_ASSERT_EQUAL_HEX8((uint8_t)(0x20 + i),
            last_write_buffer_data[HEADER_SIZE + BCAST_LEN_SIZE + i]);
    TEST_ASSERT_EQUAL_HEX8(CMD_DATA,
        atomic_load_explicit(&bcast_reply_cmd, memory_order_relaxed));
}

/* Round-trip: a CMD_DATA unformatted frame (VARA beacon) run through TX
 * framing then RX extraction must be delivered as CMD_DATA with the exact
 * original length, even on a receive-only station (reply_cmd at its default). */
/* hermes-broadcast sends CONFIG frames RQ_HEADER_SIZE short of a full modem
 * frame (transmitter.c: payload is packet_size + RQ_HEADER_SIZE, config is
 * packet_size alone) and relies on Mercury zero-padding them back up.  Its
 * receiver then discards anything that is not exactly frame_size, so if
 * Mercury wraps a short config frame instead of passing it raw, the far side
 * never receives the RaptorQ configuration and can never start decoding.
 *
 * With CMD_MODEM_FRAME the sender says which it is, so neither length nor
 * content has to be a discriminator. */
void test_bcast_short_cmd_data_is_framed(void)
{
    const size_t fsz = 10;
    broadcast_frame_size_cfg = fsz;

    uint8_t frame[MAX_PAYLOAD];
    memset(frame, 0, sizeof(frame));
    frame[0] = BCAST_HDR_RAW_CONTROL;   /* PACKET_RQ_CONFIG, extension 0 */
    frame[1] = 0xAA;
    frame[2] = 0xBB;

    /* As CMD_DATA it is a message: framed, exact length recoverable. */
    TEST_ASSERT_TRUE(bcast_process_decoded_frame(frame, 3, CMD_DATA, fsz));
    TEST_ASSERT_EQUAL_size_t(fsz, last_write_buffer_len);

    uint8_t *payload = NULL;
    int plen = 0;
    bcast_get_tx_payload(last_write_buffer_data, fsz, &payload, &plen);
    TEST_ASSERT_EQUAL_INT(3, plen);
    TEST_ASSERT_EQUAL_HEX8(BCAST_HDR_RAW_CONTROL, payload[0]);
    TEST_ASSERT_EQUAL_HEX8(0xAA, payload[1]);
    TEST_ASSERT_EQUAL_HEX8(0xBB, payload[2]);

    /* The SAME payload declared as a modem frame is passed through and padded,
     * which is how hermes-broadcast gets its short config frame on the air. */
    memset(frame, 0, sizeof(frame));
    frame[0] = BCAST_HDR_RAW_CONTROL;
    frame[1] = 0xAA;
    frame[2] = 0xBB;
    TEST_ASSERT_TRUE(bcast_process_decoded_frame(frame, 3, CMD_MODEM_FRAME, fsz));
    TEST_ASSERT_EQUAL_size_t(fsz, last_write_buffer_len);
    TEST_ASSERT_EQUAL_HEX8(BCAST_HDR_RAW_CONTROL, last_write_buffer_data[0]);
    TEST_ASSERT_EQUAL_HEX8(0xAA, last_write_buffer_data[1]);
    TEST_ASSERT_EQUAL_HEX8(0xBB, last_write_buffer_data[2]);
    for (size_t i = 3; i < fsz; i++)
        TEST_ASSERT_EQUAL_HEX8(0x00, last_write_buffer_data[i]);
}

/* A beacon whose first byte lands in the broadcast-type range but carries a
 * non-zero extension is still an unformatted payload and must be wrapped.
 * 'a' (0x61) is type 3 with extension 1 -- the case the type-bits-only test
 * got wrong, and the one a text beacon is most likely to hit. */
void test_bcast_beacon_broadcast_type_nonzero_ext_wrapped(void)
{
    const size_t fsz = 10;
    broadcast_frame_size_cfg = fsz;

    uint8_t frame[MAX_PAYLOAD];
    memset(frame, 0, sizeof(frame));
    frame[0] = 0x61;   /* 'a' : type 3, extension 1 */
    frame[1] = 'b';
    frame[2] = 'c';

    bool ok = bcast_process_decoded_frame(frame, 3, CMD_DATA, fsz);

    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_HEX8(BCAST_HDR_BYTE_DATA, last_write_buffer_data[0]);
    TEST_ASSERT_EQUAL_HEX8(0x00, last_write_buffer_data[1]);
    TEST_ASSERT_EQUAL_HEX8(0x03, last_write_buffer_data[2]);
    TEST_ASSERT_EQUAL_HEX8(0x61, last_write_buffer_data[HEADER_SIZE + BCAST_LEN_SIZE]);
}

void test_bcast_data_lenprefix_roundtrip(void)
{
    const size_t fsz = 32;
    broadcast_frame_size_cfg = fsz;

    const int orig_len = 11;
    uint8_t orig[32];
    for (int i = 0; i < orig_len; i++) orig[i] = (uint8_t)(0x20 + i);

    uint8_t txframe[MAX_PAYLOAD];
    memset(txframe, 0, sizeof(txframe));
    memcpy(txframe, orig, orig_len);
    bool ok = bcast_process_decoded_frame(txframe, orig_len, CMD_DATA, fsz);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_size_t(fsz, last_write_buffer_len);
    TEST_ASSERT_EQUAL_HEX8(BCAST_HDR_BYTE_DATA, last_write_buffer_data[0]);

    uint8_t rxframe[MAX_PAYLOAD];
    memcpy(rxframe, last_write_buffer_data, fsz);
    atomic_store_explicit(&bcast_reply_cmd, CMD_DATA, memory_order_relaxed);

    uint8_t *payload = NULL;
    int plen = 0;
    uint8_t cmd = bcast_get_tx_payload(rxframe, fsz, &payload, &plen);

    TEST_ASSERT_EQUAL_HEX8(CMD_DATA, cmd);               /* 0x02, mirrored */
    TEST_ASSERT_EQUAL_INT(orig_len, plen);
    TEST_ASSERT_EQUAL_PTR(rxframe + HEADER_SIZE + BCAST_LEN_SIZE, payload);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(orig, payload, orig_len);
}

/* A host that ASKS for telemetry must be answered, every time.
 *
 * This guards a rate-limit that was briefly added to tnc_send_sn()/
 * tnc_send_bitrate() to stop them "flooding" the control queue.  The on-demand
 * SN and BITRATE commands are answered by calling those same functions, so the
 * limiter swallowed the reply -- and since the decoder holds the window open on
 * a live link, the host was answered essentially never.  VARA hosts poll these.
 *
 * The limiter was unnecessary anyway: SN/BITRATE are emitted from
 * process_received_frame(), i.e. only on a successfully DECODED frame, so at
 * most a couple per frame and well under 1/s.  Overflowing the 256-slot queue,
 * drained every 100 ms, needs ~2560/s.  The queue only fills when the drain
 * stops, which was the real bug. */
void test_sn_query_is_always_answered(void)
{
    memset(last_queued_line, 0, sizeof(last_queued_line));

    char cmd[] = "SN";
    execute_control_command(cmd);

    /* Assert that a line came back, not which value: the cached SN depends on
     * whatever ran before this test. */
    TEST_ASSERT_TRUE_MESSAGE(strncmp(last_queued_line, "SN ", 3) == 0,
        "host asked for SN and got nothing: is the reply rate-limited?");
}

void test_bitrate_query_is_always_answered(void)
{
    memset(last_queued_line, 0, sizeof(last_queued_line));

    char cmd[] = "BITRATE";
    execute_control_command(cmd);

    TEST_ASSERT_TRUE_MESSAGE(strncmp(last_queued_line, "BITRATE ", 8) == 0,
        "host asked for BITRATE and got nothing: is the reply rate-limited?");
}

/* ---- HISTORY (persisted chat history) command tests ---- */

void test_cmd_history_empty(void)
{
    mock_msg_store_count = 0;
    char cmd[] = "HISTORY";
    execute_control_command(cmd);

    /* HISTORY 0 + HISTORYEND, two direct writes. */
    TEST_ASSERT_EQUAL(2, tcp_write_call_count);
    TEST_ASSERT_EQUAL_STRING("HISTORYEND\r", (char *)last_tcp_write_buf);
}

void test_cmd_history_with_messages(void)
{
    mock_msg_store_count = 1;
    snprintf(mock_msg_lines[0], sizeof(mock_msg_lines[0]),
             "{\"plane\":\"arq\",\"text\":\"hi\"}");
    char cmd[] = "HISTORY";
    execute_control_command(cmd);

    /* HISTORY 1 + one HISTORYMSG + HISTORYEND. */
    TEST_ASSERT_EQUAL(3, tcp_write_call_count);
    TEST_ASSERT_EQUAL_STRING("HISTORYEND\r", (char *)last_tcp_write_buf);
}

int main(void)
{
    UNITY_BEGIN();
    /* Command parser tests */
    RUN_TEST(test_cmd_mycall);
    RUN_TEST(test_cmd_mycall_registered_follows_ok);
    RUN_TEST(test_cmd_mycall_registers_every_callsign);
    RUN_TEST(test_cmd_mycall_does_not_register_dropped_secondaries);
    RUN_TEST(test_cmd_listen_on);
    RUN_TEST(test_cmd_listen_off);
    RUN_TEST(test_cmd_public_on);
    RUN_TEST(test_cmd_compression);
    RUN_TEST(test_cmd_tune_on);
    RUN_TEST(test_cmd_tune_on_fractional_level);
    RUN_TEST(test_cmd_tune_off);
    RUN_TEST(test_cmd_tune_query_reports_level);
    RUN_TEST(test_cmd_tune_refusal_is_not_ok);
    RUN_TEST(test_cmd_tune_missing_argument);
    RUN_TEST(test_cmd_tune_garbage_argument);
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
    RUN_TEST(test_tnc_send_registered);
    /* Broadcast framing helper tests */
    RUN_TEST(test_bcast_rx_cmd_data_exact_size);
    RUN_TEST(test_bcast_rx_cmd_data_0x60_is_not_special);
    RUN_TEST(test_bcast_rx_cmd_data_short_lowercase_wrapped);
    RUN_TEST(test_bcast_rx_cmd_data_oversized_discarded);
    RUN_TEST(test_bcast_rx_vara_header_injected);
    RUN_TEST(test_bcast_rx_cmd_ax25_reply_cmd);
    RUN_TEST(test_bcast_rx_vara_long_payload_truncated);
    RUN_TEST(test_bcast_tx_cmd_data_full_frame);
    RUN_TEST(test_bcast_tx_vara_strips_header);
    RUN_TEST(test_bcast_vara_length_roundtrip);
    RUN_TEST(test_bcast_tx_modem_frame_command_passes_through_untouched);
    RUN_TEST(test_bcast_tx_same_frame_as_chat_is_truncated);
    RUN_TEST(test_bcast_tx_full_size_chat_is_still_wrapped);
    RUN_TEST(test_bcast_tx_full_size_cmd_data_is_framed);
    RUN_TEST(test_bcast_tx_lenprefix_ignores_reply_cmd_default);
    RUN_TEST(test_bcast_std_kiss_roundtrip);
    RUN_TEST(test_bcast_rx_cmd_data_vara_beacon_wrapped);
    RUN_TEST(test_bcast_short_cmd_data_is_framed);
    RUN_TEST(test_bcast_beacon_broadcast_type_nonzero_ext_wrapped);
    RUN_TEST(test_bcast_data_lenprefix_roundtrip);
    RUN_TEST(test_sn_query_is_always_answered);
    RUN_TEST(test_bitrate_query_is_always_answered);
    /* HISTORY command tests */
    RUN_TEST(test_cmd_history_empty);
    RUN_TEST(test_cmd_history_with_messages);
    return UNITY_END();
}
