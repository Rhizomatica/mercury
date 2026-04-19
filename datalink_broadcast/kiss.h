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


#define CMD_UNKNOWN 0xFE
#define CMD_AX25 0x00 //  AX25 Frame (standard) in VARA
#define CMD_AX25CALLSIGN 0x01 // AX25 Frame (7 chrs Call Signs) in VARA
#define CMD_DATA 0x02 // VARA / Mercury unformatted frame
#define CMD_RQ_CONFIG 0x03 // Mercury special fountain code configuration frame
#define CMD_RQ_PAYLOAD 0x04 // Fountain code payload

#define MAX_PAYLOAD 756 // ~ 18 frames at VARA Level 4

void kiss_reset_state(void);

int kiss_read(uint8_t sbyte, uint8_t *frame_buffer);

uint8_t kiss_last_command(void);

int kiss_write_frame(uint8_t* buffer, int frame_len, uint8_t cmd, uint8_t* write_buffer);


#ifdef __cplusplus
};
#endif
