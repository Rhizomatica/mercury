/* tests/sim/sim_translate.c
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Rhizomatica
 *
 * Port of arq_handle_incoming_frame (arq.c:649-748) and
 * arq_handle_incoming_connect_frame (arq.c:569-616) into a standalone
 * translator that targets an arq_event_t output instead of evq_push,
 * and takes the receiver callsign explicitly. */

#include "sim_translate.h"
#include "arq_protocol.h"
#include "arq.h"        /* CALLSIGN_MAX_SIZE */
#include "framer.h"     /* PACKET_TYPE_ARQ_*, frame_header_packet_type */
#include "freedv_api.h" /* FREEDV_MODE_DATAC15 and friends */

#include <math.h>
#include <string.h>
#include <stdio.h>

bool sim_translate_frame(const uint8_t *frame, size_t frame_size, float rx_snr,
                         const char *receiver_call, arq_event_t *out_ev)
{
    if (!frame || !out_ev || frame_size < 1)
        return false;

    memset(out_ev, 0, sizeof(*out_ev));

    uint8_t ptype = frame_header_packet_type(frame[0]);

    /* ---- CALL / ACCEPT frames (compact 14-byte layout) ---- */
    if (ptype == PACKET_TYPE_ARQ_CALL2)
    {
        if (frame_size < 2)
            return false;

        bool is_accept = (frame[ARQ_CONNECT_SESSION_IDX] & ARQ_CONNECT_ACCEPT_FLAG) != 0;

        uint8_t session_id = 0;
        char src[CALLSIGN_MAX_SIZE] = {0};
        int  bw_hz = 0;
        bool tx_mfsk = false;
        arq_accept_t acc;
        int rc = !arq_protocol_connect_version_ok(frame, frame_size) ? -1
                 : is_accept ? arq_protocol_parse_accept(frame, frame_size, &acc)
                             : arq_protocol_parse_call(frame, frame_size, &session_id, src, &bw_hz, &tx_mfsk);
        if (rc < 0)
            return false;
        if (is_accept) {
            session_id = acc.session_id;
            tx_mfsk    = acc.tx_mfsk;
        }

        /* Validate DST CRC against the receiver's callsign (all 16 bits). */
        if (receiver_call && receiver_call[0] != 0 &&
            !arq_protocol_connect_dst_matches(frame, frame_size, receiver_call))
            return false;

        out_ev->id         = is_accept ? ARQ_EV_RX_ACCEPT : ARQ_EV_RX_CALL;
        out_ev->session_id = session_id;
        out_ev->rx_snr     = rx_snr;
        out_ev->tx_mfsk    = tx_mfsk;
        if (is_accept) {
            out_ev->car_level      = acc.start_level;
            out_ev->car_ctl_deaf   = acc.ctl_deaf;
            out_ev->car_call_snr   = isnan(acc.call_snr_db) ? 0.0f : acc.call_snr_db;
            out_ev->car_nonce      = acc.nonce;
            out_ev->car_called_crc = acc.called_crc;
        }
        /* src = transmitting side's callsign (the remote station for the receiver) */
        snprintf(out_ev->remote_call, CALLSIGN_MAX_SIZE, "%s", src);
        return true;
    }

    /* ---- DATA and CONTROL frames (8-byte header layout) ---- */
    if (frame_size < ARQ_FRAME_HDR_SIZE)
        return false;

    arq_frame_hdr_t hdr;
    if (arq_protocol_decode_hdr(frame, frame_size, &hdr) < 0)
        return false;

    /* The one plain session frame: the DISCONNECT (as arq_handle_incoming_frame). */
    if (hdr.packet_type != PACKET_TYPE_ARQ_CONTROL || hdr.subtype != ARQ_SUBTYPE_DISCONNECT)
        return false;
    out_ev->id         = ARQ_EV_RX_DISCONNECT;
    out_ev->session_id = hdr.session_id;
    out_ev->rx_snr     = rx_snr;
    return true;
}
