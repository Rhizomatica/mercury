/* Mercury monitor-mode frame reporter
 *
 * Turns a CRC-valid decoded frame into a "MONITOR <mode> <kind> <detail>
 * SNR=<snr>" line for the host.  The monitor is read-only: its decoders are
 * separate instances that never feed the ARQ FSM, so nothing is acknowledged
 * and the transmitter is never keyed on account of what is heard here.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>

#include "freedv_api.h"
#include "modem_mfsk.h"
#include "framer.h"
#include "arq.h"
#include "arq_protocol.h"
#include "tcp_interfaces.h"

#include "monitor_frame.h"

const char *mode_name_from_enum(int mode)
{
    switch (mode)
    {
    case FREEDV_MODE_DATAC1: return "DATAC1";
    case FREEDV_MODE_DATAC3: return "DATAC3";
    case FREEDV_MODE_DATAC0: return "DATAC0";
    case FREEDV_MODE_DATAC4: return "DATAC4";
    case FREEDV_MODE_DATAC13: return "DATAC13";
    case FREEDV_MODE_DATAC14: return "DATAC14";
    case FREEDV_MODE_DATAC15: return "DATAC15";
    case FREEDV_MODE_DATAC16: return "DATAC16";
    case FREEDV_MODE_DATAC17: return "DATAC17";
    case FREEDV_MODE_QAM16C2: return "QAM16C2";
    case FREEDV_MODE_FSK_LDPC: return "FSK_LDPC";
    case MERCURY_MODE_MFSK: return "MFSK";
    default: return "UNKNOWN";
    }
}

static const char *monitor_subtype_name(uint8_t subtype)
{
    switch (subtype)
    {
    case ARQ_SUBTYPE_CALL:          return "CALL";
    case ARQ_SUBTYPE_ACCEPT:        return "ACCEPT";
    case ARQ_SUBTYPE_ACK:           return "ACK";
    case ARQ_SUBTYPE_DISCONNECT:    return "DISCONNECT";
    case ARQ_SUBTYPE_DATA:          return "DATA";
    case ARQ_SUBTYPE_KEEPALIVE:     return "KEEPALIVE";
    case ARQ_SUBTYPE_KEEPALIVE_ACK: return "KEEPALIVE_ACK";
    case ARQ_SUBTYPE_MODE_REQ:      return "MODE_REQ";
    case ARQ_SUBTYPE_MODE_ACK:      return "MODE_ACK";
    case ARQ_SUBTYPE_TURN_REQ:      return "TURN_REQ";
    case ARQ_SUBTYPE_TURN_ACK:      return "TURN_ACK";
    default:                        return "CONTROL";
    }
}

void process_monitor_frame(const uint8_t *data, size_t nbytes_out,
                           int mode, float snr_est)
{
    size_t payload_nbytes;
    int frame_type;
    const char *mode_name = mode_name_from_enum(mode);
    char detail[96];

    if (!data || nbytes_out < 2)
        return;

    payload_nbytes = nbytes_out - 2;
    if (payload_nbytes == 0)
        return;

    frame_type = parse_frame_header(data, payload_nbytes, NULL);

    switch (frame_type)
    {
    case PACKET_TYPE_ARQ_CALL:
    {
        bool is_accept = (data[ARQ_CONNECT_SESSION_IDX] & ARQ_CONNECT_ACCEPT_FLAG) != 0;
        char src[CALLSIGN_MAX_SIZE] = {0};
        char dst[CALLSIGN_MAX_SIZE] = {0};   /* not recoverable: only its CRC is on the wire */
        uint8_t sid = 0;
        int bw = 0;
        int rc = is_accept
                   ? arq_protocol_parse_accept(data, payload_nbytes, &sid, src, dst, &bw)
                   : arq_protocol_parse_call(data, payload_nbytes, &sid, src, dst, &bw);
        if (rc == 0)
            snprintf(detail, sizeof(detail), "FROM=%s BW=%d SID=%u",
                     src, bw, (unsigned)sid);
        else
            snprintf(detail, sizeof(detail), "FROM=?");
        tnc_send_monitor(mode_name, is_accept ? "ACCEPT" : "CALL", detail, snr_est);
        break;
    }
    case PACKET_TYPE_ARQ_CQ:
    {
        char src[CALLSIGN_MAX_SIZE] = {0};
        int bw = 0;
        if (arq_protocol_parse_cq(data, payload_nbytes, src, &bw) == 0)
            snprintf(detail, sizeof(detail), "FROM=%s BW=%d", src, bw);
        else
            snprintf(detail, sizeof(detail), "FROM=?");
        tnc_send_monitor(mode_name, "CQ", detail, snr_est);
        break;
    }
    case PACKET_TYPE_ARQ_CONTROL:
    case PACKET_TYPE_ARQ_DATA:
    {
        arq_frame_hdr_t hdr;
        if (arq_protocol_decode_hdr(data, payload_nbytes, &hdr) != 0)
        {
            tnc_send_monitor(mode_name, "FRAME", "unparseable", snr_est);
            break;
        }
        size_t user_len = (payload_nbytes > ARQ_FRAME_HDR_SIZE)
                              ? payload_nbytes - ARQ_FRAME_HDR_SIZE : 0;
        char hex[64] = {0};
        if (frame_type == PACKET_TYPE_ARQ_DATA && user_len > 0)
        {
            size_t n = user_len < 16 ? user_len : 16;
            for (size_t i = 0; i < n; i++)
                snprintf(hex + 2 * i, sizeof(hex) - 2 * i, "%02X",
                         data[ARQ_FRAME_HDR_SIZE + i]);
        }
        if (frame_type == PACKET_TYPE_ARQ_DATA)
            snprintf(detail, sizeof(detail), "SID=%u SEQ=%u ACK=%u LEN=%zu%s%s",
                     (unsigned)hdr.session_id, (unsigned)hdr.tx_seq,
                     (unsigned)hdr.rx_ack_seq, user_len,
                     hex[0] ? " HEX=" : "", hex);
        else
            snprintf(detail, sizeof(detail), "TYPE=%s SID=%u SEQ=%u ACK=%u",
                     monitor_subtype_name(hdr.subtype),
                     (unsigned)hdr.session_id, (unsigned)hdr.tx_seq,
                     (unsigned)hdr.rx_ack_seq);
        tnc_send_monitor(mode_name,
                         frame_type == PACKET_TYPE_ARQ_DATA
                             ? "DATA" : monitor_subtype_name(hdr.subtype),
                         detail, snr_est);
        break;
    }
    case PACKET_TYPE_BROADCAST_CONTROL:
    case PACKET_TYPE_BROADCAST_DATA:
        snprintf(detail, sizeof(detail), "LEN=%zu", payload_nbytes);
        tnc_send_monitor(mode_name,
                         frame_type == PACKET_TYPE_BROADCAST_CONTROL
                             ? "BCAST_CTRL" : "BCAST_DATA",
                         detail, snr_est);
        break;
    default:
        tnc_send_monitor(mode_name, "FRAME", "unknown-type", snr_est);
        break;
    }
}
