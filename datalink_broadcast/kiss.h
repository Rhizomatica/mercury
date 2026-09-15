/* KISS framer
 *
 * Copyright (C) 2020-2024 Rhizomatica
 * Author: Rafael Diniz <rafael@rhizomatica.org>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FEND 0xC0
#define FESC 0xDB
#define TFEND 0xDC
#define TFESC 0xDD



//                 VARA KISS Frame types

//    192  0  |... AX25 Frame (standard)          ...... |  192    
//    192  1  |... AX25 Frame (7 chrs Call Signs) ... |  192        
//    192  2  |... Unformatted frame              ........ |  192


// If you send callsigns origin and destination in the frames it's better to use KISS Type 1
// and follow the AX25 structure instead of KISS Type2, as the frame is specially compressed
// and the final size it is greater than unformatted KISS.       by Ros <-
//
// KISS command bytes describe the outer TCP/TNC framing only.
//
// In the current Mercury <-> hermes-broadcast TCP path, raw modem frames are
// normally carried inside CMD_DATA, and the first byte of the decoded modem
// frame carries the Mercury packet type (see modem/framer.h).
//
// So broadcast-vs-ARQ classification happens from frame[0] after KISS
// decoding, not from the KISS command byte. CMD_RQ_* are kept here as
// reserved/legacy KISS command-space values; do not confuse them with Mercury
// packet-type values such as PACKET_TYPE_BROADCAST_*.


#define CMD_UNKNOWN 0xFE
#define CMD_AX25 0x00 //  AX25 Frame (standard) in VARA
#define CMD_AX25CALLSIGN 0x01 // AX25 Frame (7 chrs Call Signs) in VARA
#define CMD_DATA 0x02 // Raw/unformatted KISS payload; current hermes-broadcast TCP framing uses this
/* "The payload is already exactly one modem frame -- pass it through."
 *
 * This exists because the alternative is guessing.  Mercury has to decide
 * whether a client's payload is a message to be framed (header + length prefix
 * injected) or a modem frame to be transmitted untouched, and with only
 * CMD_DATA to go on it inferred that from the payload's own first byte -- which
 * a sender is free to choose.  A full-size payload beginning 0x60..0x9F looked
 * like a modem frame whatever it actually was, and conversely a genuine modem
 * frame sent as CMD_AX25 was silently truncated by 3 bytes to make room for a
 * header it did not need.
 *
 * A sender that knows what it is sending should say so.  0x03 was already
 * reserved and unused as a KISS command, so it costs nothing.
 *
 * CMD_DATA keeps its inference and stays fully supported -- it is what older
 * builds send.  CMD_AX25 and CMD_AX25CALLSIGN are untouched: they always mean
 * "a message, frame it", which is what VARA clients depend on. */
#define CMD_MODEM_FRAME 0x03

/* The RaptorQ packet type lives in the FRAME's header byte, never in the KISS
 * command.  Kept so older code referring to these names still compiles. */
#define CMD_RQ_CONFIG 0x03
#define CMD_RQ_PAYLOAD 0x04 // Reserved/legacy KISS command value; current TCP framing does not use it for RaptorQ payload

#define MAX_PAYLOAD 1213 // largest broadcast frame we can select (QAM16C2)

void kiss_reset_state(void);

int kiss_read(uint8_t sbyte, uint8_t *frame_buffer);

uint8_t kiss_last_command(void);

int kiss_write_frame(uint8_t* buffer, int frame_len, uint8_t cmd, uint8_t* write_buffer);


#ifdef __cplusplus
};
#endif
