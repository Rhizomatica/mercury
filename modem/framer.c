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

#include <stdio.h>
#include "framer.h"

/*
 * Framer byte layout (v4):
 *   bits [7:5] = packet_type  (3 bits, PACKET_TYPE_* values)
 *   bits [4:0] = extension    (packet-type-specific metadata)
 */

int8_t parse_frame_header(const uint8_t *data_frame, uint32_t frame_size, uint8_t *extension_out)
{
    if (!data_frame || frame_size < 1)
        return -1;

    if (extension_out)
        *extension_out = frame_header_extension(data_frame[0]);

    return (int8_t)frame_header_packet_type(data_frame[0]);
}

void write_frame_header(uint8_t *data, int packet_type, uint8_t extension)
{
    if (!data)
        return;

    data[0] = (uint8_t)(((packet_type & PACKET_TYPE_MASK) << PACKET_TYPE_SHIFT) |
                        (extension & FRAME_EXT_MASK));
}
