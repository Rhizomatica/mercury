/* tests/sim/sim_endpoint.c
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Rhizomatica */
#include "sim_endpoint.h"
#include "arq.h"    /* arq_conn, CALLSIGN_MAX_SIZE, ARQ_BANDWIDTH_FULL_HZ */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>

#define SIM_TX_CAP   (256 * 1024)
#define SIM_RX_CAP   (256 * 1024)
/* SIM_OUTBOX_MAX lives in sim_endpoint.h (shared with the core). */

struct sim_endpoint {
    arq_session_t sess;
    char my_call[CALLSIGN_MAX_SIZE];
    char peer_call[CALLSIGN_MAX_SIZE];

    uint8_t tx[SIM_TX_CAP]; size_t tx_head, tx_len;   /* app bytes to send   */
    uint8_t rx[SIM_RX_CAP]; size_t rx_len;            /* delivered app bytes */

    /* FIFO of frames emitted by send_tx_frame/send_pattern_ack in one dispatch,
     * drained by the core. A windowed sender emits K DATA frames of one keydown
     * back-to-back (burst_remaining counts down to 0 on the last); stop-and-wait
     * emits a single frame (burst_remaining==0) — the degenerate case. */
    sim_outframe_t outbox[SIM_OUTBOX_MAX];
    int            outbox_head, outbox_count;
};

static void outbox_push(sim_endpoint_t *ep, const sim_outframe_t *of)
{
    assert(ep->outbox_count < SIM_OUTBOX_MAX && "sim outbox overflow");
    int tail = (ep->outbox_head + ep->outbox_count) % SIM_OUTBOX_MAX;
    ep->outbox[tail] = *of;
    ep->outbox_count++;
}

/* v1 context: set before each arq_fsm_dispatch. Also updates arq_conn.my_call_sign
 * so send_call_accept() fills the correct SRC callsign in CALL/ACCEPT frames. */
static sim_endpoint_t *s_active;

void sim_endpoint_set_active(sim_endpoint_t *ep)
{
    s_active = ep;
    if (ep) {
        snprintf(arq_conn.my_call_sign, sizeof(arq_conn.my_call_sign),
                 "%s", ep->my_call);
        arq_conn.bw = ARQ_BANDWIDTH_FULL_HZ;
    }
}

sim_endpoint_t *sim_endpoint_active(void)                    { return s_active; }
arq_session_t  *sim_endpoint_session(sim_endpoint_t *ep)     { return &ep->sess; }
const char     *sim_endpoint_call(sim_endpoint_t *ep)        { return ep->my_call; }

sim_endpoint_t *sim_endpoint_create(const char *my_call, const char *peer_call)
{
    sim_endpoint_t *ep = calloc(1, sizeof(*ep));
    if (!ep) return NULL;
    snprintf(ep->my_call,   sizeof(ep->my_call),   "%s", my_call);
    snprintf(ep->peer_call, sizeof(ep->peer_call), "%s", peer_call);
    arq_fsm_init(&ep->sess);
    return ep;
}

void sim_endpoint_destroy(sim_endpoint_t *ep) { free(ep); }

void sim_endpoint_queue_tx(sim_endpoint_t *ep, const uint8_t *data, size_t len)
{
    assert(ep->tx_len + len <= SIM_TX_CAP);
    memcpy(ep->tx + ep->tx_len, data, len);
    ep->tx_len += len;
}

size_t sim_endpoint_delivered(sim_endpoint_t *ep, uint8_t *out, size_t out_cap)
{
    size_t n = ep->rx_len < out_cap ? ep->rx_len : out_cap;
    if (out && n > 0)
        memcpy(out, ep->rx, n);
    return n;
}

bool sim_endpoint_take_outframe(sim_endpoint_t *ep, sim_outframe_t *out)
{
    if (ep->outbox_count == 0) return false;
    *out = ep->outbox[ep->outbox_head];
    ep->outbox_head = (ep->outbox_head + 1) % SIM_OUTBOX_MAX;
    ep->outbox_count--;
    return true;
}

/* ---- the nine shared FSM callbacks: all operate on s_active ---- */

static void cb_send_tx_frame(int packet_type, int mode, size_t frame_size,
                              const uint8_t *frame, int burst_remaining)
{
    sim_endpoint_t *ep = s_active;
    /* Windowed sender: K DATA frames of one keydown are emitted back-to-back,
     * burst_remaining counting down to 0 on the last; the core groups them into
     * one keydown (one preamble/turnaround). Stop-and-wait emits one frame with
     * burst_remaining==0 — the degenerate single-frame keydown. */
    sim_outframe_t of;
    memset(&of, 0, sizeof(of));
    assert(frame_size <= sizeof(of.buf));
    memcpy(of.buf, frame, frame_size);
    of.len             = frame_size;
    of.packet_type     = packet_type;
    of.mode            = mode;
    of.burst_remaining = burst_remaining;
    of.is_pattern      = false;
    of.pattern_kind    = 0;
    of.present         = true;
    outbox_push(ep, &of);
}

/* Count of epoch-TAGGED (fast windowed) pattern ACKs emitted across all
 * endpoints — lets a test confirm the fast-ACK path was actually exercised
 * (vs the coded-SACK fallback), not merely that the transfer completed. */
static int s_tagged_pattern_count = 0;
int  sim_tagged_pattern_count(void) { return s_tagged_pattern_count; }
void sim_tagged_pattern_reset(void) { s_tagged_pattern_count = 0; }

/* Pattern ACK: a degenerate outframe with no coded bytes.  sim_core schedules
 * it with a short airtime and its own low erasure, and translates it straight
 * to ARQ_EV_RX_ACK (HAS_DATA = break) on delivery. */
static void cb_send_pattern_ack(int mode, int pattern_kind)
{
    sim_endpoint_t *ep = s_active;
    sim_outframe_t of;
    memset(&of, 0, sizeof(of));
    of.len             = 0;
    of.packet_type     = -1;
    of.mode            = mode;
    of.burst_remaining = 0;
    of.is_pattern      = true;
    of.pattern_kind    = pattern_kind;
    of.present         = true;
    if (pattern_kind & ARQ_PATTERN_TAGGED)
        s_tagged_pattern_count++;
    outbox_push(ep, &of);
}

static void cb_notify_connected(const char *remote_call, const char *local_call) { (void)remote_call; (void)local_call; }
static void cb_notify_pending(const char *remote_call, const char *local_call)    { (void)remote_call; (void)local_call; }
static void cb_notify_cancelpending(void)                   { }
static void cb_notify_disconnected(bool to_no_client)       { (void)to_no_client; }

static void cb_deliver_rx_data(const uint8_t *data, size_t len)
{
    sim_endpoint_t *ep = s_active;
    if (ep->rx_len + len > SIM_RX_CAP) len = SIM_RX_CAP - ep->rx_len;
    memcpy(ep->rx + ep->rx_len, data, len);
    ep->rx_len += len;
}

static int cb_tx_backlog(void)
{
    sim_endpoint_t *ep = s_active;
    return (int)(ep->tx_len - ep->tx_head);
}

static int cb_tx_read(uint8_t *buf, size_t len)
{
    sim_endpoint_t *ep = s_active;
    size_t avail = ep->tx_len - ep->tx_head;
    if (len > avail) len = avail;
    memcpy(buf, ep->tx + ep->tx_head, len);
    ep->tx_head += len;
    return (int)len;
}

static void cb_send_buffer_status(int backlog_bytes) { (void)backlog_bytes; }

const arq_fsm_callbacks_t *sim_endpoint_callbacks(void)
{
    static const arq_fsm_callbacks_t cbs = {
        .send_tx_frame        = cb_send_tx_frame,
        .send_pattern_ack     = cb_send_pattern_ack,
        .notify_connected     = cb_notify_connected,
        .notify_pending       = cb_notify_pending,
        .notify_cancelpending = cb_notify_cancelpending,
        .notify_disconnected  = cb_notify_disconnected,
        .deliver_rx_data      = cb_deliver_rx_data,
        .tx_backlog           = cb_tx_backlog,
        .tx_read              = cb_tx_read,
        .send_buffer_status   = cb_send_buffer_status,
    };
    return &cbs;
}
