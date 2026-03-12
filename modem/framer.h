/* HERMES Modem
 *
 * Copyright (C) 2025 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3, or (at your option)
 * any later version.
 *
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 */

#include <stdint.h>
#include <stddef.h>

#ifndef FRAMER_H
#define FRAMER_H

#define PACKET_TYPE_ARQ_CONTROL       0x00  /* ACK, DISCONNECT, TURN_REQ, etc.    */
#define PACKET_TYPE_ARQ_DATA          0x01  /* data payload frames                */
#define PACKET_TYPE_ARQ_CALL          0x02  /* CALL/ACCEPT setup (compact layout) */
#define PACKET_TYPE_BROADCAST_CONTROL 0x03  /* (was 0x02 in v2)                   */
#define PACKET_TYPE_BROADCAST_DATA    0x04  /* (was 0x03 in v2)                   */
#define PACKET_TYPE_ARQ_CQ            0x05  /* compact DATAC13 CQ metadata frame  */

#define PACKET_TYPE_BITS   3    /* bits [7:5] of framer byte */
#define PACKET_TYPE_SHIFT  5
#define PACKET_TYPE_MASK   0x07
#define FRAME_EXT_BITS     5    /* bits [4:0] of framer byte */
#define FRAME_EXT_MASK     0x1f

#define HEADER_SIZE 1 // Size of the Hermes header
#define BROADCAST_CONFIG_PACKET_SIZE 9 // hermes-broadcast RaptorQ config packet

static inline uint8_t frame_header_packet_type(uint8_t header)
{
    return (uint8_t)((header >> PACKET_TYPE_SHIFT) & PACKET_TYPE_MASK);
}

static inline uint8_t frame_header_extension(uint8_t header)
{
    return (uint8_t)(header & FRAME_EXT_MASK);
}

// Parse the frame header and optionally return the extension field.
// Returns packet type or negative on invalid input.
int8_t parse_frame_header(const uint8_t *data_frame, uint32_t frame_size, uint8_t *extension_out);
void write_frame_header(uint8_t *data, int packet_type, uint8_t extension);




#endif // FRAMER_H
