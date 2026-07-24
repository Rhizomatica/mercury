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

#include <string.h>
#include <stdio.h>

bool sim_translate_frame(const uint8_t *frame, size_t frame_size, float rx_snr,
                         const char *receiver_call, arq_event_t *out_ev)
{
    if (!frame || !out_ev || frame_size < 1)
        return false;

    memset(out_ev, 0, sizeof(*out_ev));
    out_ev->ack_epoch  = -1;   /* coded frames carry no epoch (tagged patterns do) */
    out_ev->data_epoch = -1;

    uint8_t ptype = frame_header_packet_type(frame[0]);

    /* ---- CALL / ACCEPT frames (compact 14-byte layout) ---- */
    if (ptype == PACKET_TYPE_ARQ_CALL)
    {
        if (frame_size < 2)
            return false;

        bool is_accept = (frame[ARQ_CONNECT_SESSION_IDX] & ARQ_CONNECT_ACCEPT_FLAG) != 0;

        uint8_t session_id;
        char src[CALLSIGN_MAX_SIZE] = {0};
        char dst[CALLSIGN_MAX_SIZE] = {0};
        int  bw_hz = 0;

        int rc = is_accept
                 ? arq_protocol_parse_accept(frame, frame_size, &session_id, src, dst, &bw_hz)
                 : arq_protocol_parse_call  (frame, frame_size, &session_id, src, dst, &bw_hz);
        if (rc < 0)
            return false;

        /* Validate DST CRC against the receiver's callsign. */
        if (receiver_call && receiver_call[0] != 0)
        {
            uint16_t frame_crc = (uint16_t)frame[ARQ_CONNECT_PAYLOAD_IDX]
                               | ((uint16_t)frame[ARQ_CONNECT_PAYLOAD_IDX + 1] << 8);
            if (frame_crc != arq_protocol_callsign_crc16(receiver_call))
                return false;
        }

        out_ev->id         = is_accept ? ARQ_EV_RX_ACCEPT : ARQ_EV_RX_CALL;
        out_ev->session_id = session_id;
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

    out_ev->session_id    = hdr.session_id;
    out_ev->seq           = hdr.tx_seq;
    out_ev->ack_seq       = hdr.rx_ack_seq;
    out_ev->rx_flags      = hdr.flags;
    out_ev->snr_encoded   = (int8_t)hdr.snr_raw;
    out_ev->ack_delay_raw = hdr.ack_delay_raw;

    if (hdr.packet_type == PACKET_TYPE_ARQ_DATA)
    {
        out_ev->id   = ARQ_EV_RX_DATA;
        out_ev->rx_snr = rx_snr;
        out_ev->data_epoch = hdr.ack_delay_raw & 0x3;   /* keydown epoch (byte 7) */

        /* Infer the FreeDV mode from frame_size by matching the mode table
         * (DATA frames are zero-padded to the mode's full payload_bytes).
         * Mirrors arq_handle_incoming_frame in arq.c. */
        out_ev->mode = FREEDV_MODE_DATAC15;   /* safe default */
        for (int i = 0; i < arq_mode_table_count; i++)
        {
            if ((int)frame_size == arq_mode_table[i].payload_bytes)
            {
                out_ev->mode = arq_mode_table[i].freedv_mode;
                break;
            }
        }

        /* Unpack the block container: block_count blocks [seq|len|data]. */
        arq_block_t blk[ARQ_MAX_BLOCKS_PER_FRAME];
        int nb = arq_protocol_parse_data_blocks(frame, frame_size, &hdr,
                                                blk, ARQ_MAX_BLOCKS_PER_FRAME);
        if (nb < 1)
            return false;
        out_ev->nblocks = nb;
        out_ev->seq     = blk[0].seq;   /* legacy in-window classification key */
        size_t off = 0;
        for (int i = 0; i < nb; i++)
        {
            if (off + blk[i].len > sizeof(out_ev->payload))
                break;
            memcpy(out_ev->payload + off, blk[i].data, blk[i].len);
            out_ev->blocks[i].seq = blk[i].seq;
            out_ev->blocks[i].off = (uint16_t)off;
            out_ev->blocks[i].len = blk[i].len;
            off += blk[i].len;
        }
        out_ev->payload_len = off;
        out_ev->data_bytes  = off;
        return true;
    }

    if (hdr.packet_type == PACKET_TYPE_ARQ_CONTROL)
    {
        switch (hdr.subtype)
        {
        /* ACK: the coded post-ACCEPT connect confirmation, and the windowed
         * selective ACK (SACK: rcv_base in rx_ack_seq + one bitmap byte after
         * the header).  A CLEAN burst is acked by the MFSK pattern (a pattern
         * outframe translated directly in sim_core, not here). */
        case ARQ_SUBTYPE_ACK:
            out_ev->id = ARQ_EV_RX_ACK;
            if ((hdr.flags & ARQ_FLAG_SACK) &&
                frame_size >= ARQ_FRAME_HDR_SIZE + ARQ_SACK_BITMAP_BYTES)
            {
                out_ev->sack_present = true;
                memcpy(out_ev->sack_bitmap, frame + ARQ_FRAME_HDR_SIZE,
                       ARQ_SACK_BITMAP_BYTES);
            }
            break;
        case ARQ_SUBTYPE_DISCONNECT:    out_ev->id = ARQ_EV_RX_DISCONNECT;    break;
        default:
            return false;
        }
        return true;
    }

    return false;
}
