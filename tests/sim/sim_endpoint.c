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

struct sim_endpoint {
    arq_session_t sess;
    char my_call[CALLSIGN_MAX_SIZE];
    char peer_call[CALLSIGN_MAX_SIZE];

    uint8_t tx[SIM_TX_CAP]; size_t tx_head, tx_len;   /* app bytes to send   */
    uint8_t rx[SIM_RX_CAP]; size_t rx_len;            /* delivered app bytes */

    sim_outframe_t outbox;   /* frame emitted by send_tx_frame, drained by core */
};

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
    if (!ep->outbox.present) return false;
    *out = ep->outbox;
    ep->outbox.present = false;
    return true;
}

/* ---- the nine shared FSM callbacks: all operate on s_active ---- */

static void cb_send_tx_frame(int packet_type, int mode, size_t frame_size,
                              const uint8_t *frame, int burst_remaining)
{
    sim_endpoint_t *ep = s_active;
    /* Stop-and-wait: at most one outframe in flight at a time. The core drains
     * the outbox before the next dispatch that can emit a frame, so if this
     * fires while an outframe is present, the burst machinery has changed. */
    assert(!ep->outbox.present &&
           "send_tx_frame fired with outbox full: multi-frame burst detected");
    assert(frame_size <= sizeof(ep->outbox.buf));
    memcpy(ep->outbox.buf, frame, frame_size);
    ep->outbox.len           = frame_size;
    ep->outbox.packet_type   = packet_type;
    ep->outbox.mode          = mode;
    ep->outbox.burst_remaining = burst_remaining;
    ep->outbox.present       = true;
}

static void cb_notify_connected(const char *remote_call)    { (void)remote_call; }
static void cb_notify_pending(const char *remote_call)      { (void)remote_call; }
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
