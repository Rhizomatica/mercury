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
    case MERCURY_MODE_MFSK_N32: return "MFSK500";
    default: return "UNKNOWN";
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
    case PACKET_TYPE_ARQ_CALL2:
    {
        bool is_accept = (data[ARQ_CONNECT_SESSION_IDX] & ARQ_CONNECT_ACCEPT_FLAG) != 0;
        if (!arq_protocol_connect_version_ok(data, payload_nbytes))
            snprintf(detail, sizeof(detail), "another protocol version (Mercury 1.9.x?)");
        else if (is_accept)
        {
            /* An ACCEPT names neither callsign in full: both are CRCs. */
            arq_accept_t acc;
            if (arq_protocol_parse_accept(data, payload_nbytes, &acc) == 0)
                snprintf(detail, sizeof(detail), "BW=%d SID=%u START=%d%s",
                         acc.bw_hz, (unsigned)acc.session_id, acc.start_level,
                         acc.tx_mfsk ? " MFSK-TX" : "");
            else
                snprintf(detail, sizeof(detail), "unparseable");
        }
        else
        {
            char src[CALLSIGN_MAX_SIZE] = {0};
            uint8_t sid = 0;
            int bw = 0;
            bool tx_mfsk = false;
            if (arq_protocol_parse_call(data, payload_nbytes, &sid, src, &bw, &tx_mfsk) == 0)
                snprintf(detail, sizeof(detail), "FROM=%s BW=%d SID=%u%s",
                         src, bw, (unsigned)sid, tx_mfsk ? " MFSK-TX" : "");
            else
                snprintf(detail, sizeof(detail), "FROM=?");
        }
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
        /* A session's carousel frames carry its seed in their CRC, so they
         * never decode here.  The one plain session frame left is the
         * DISCONNECT; anything else is another protocol version's. */
        arq_frame_hdr_t hdr;
        if (frame_type == PACKET_TYPE_ARQ_CONTROL &&
            arq_protocol_decode_hdr(data, payload_nbytes, &hdr) == 0 &&
            hdr.subtype == ARQ_SUBTYPE_DISCONNECT)
        {
            snprintf(detail, sizeof(detail), "SID=%u", (unsigned)hdr.session_id);
            tnc_send_monitor(mode_name, "DISCONNECT", detail, snr_est);
        }
        else
            tnc_send_monitor(mode_name, frame_type == PACKET_TYPE_ARQ_DATA ? "DATA" : "CONTROL",
                             "another protocol version (Mercury 1.9.x?)", snr_est);
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
