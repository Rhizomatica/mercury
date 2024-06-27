/*
 * Mercury: A configurable open-source software-defined modem.
 * Copyright (C) 2022-2024 Fadi Jerji
 * Author: Fadi Jerji
 * Email: fadi.jerji@  <gmail.com, caisresearch.com, ieee.org>
 * ORCID: 0000-0002-2076-5831
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, version 3 of the
 * License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 */

#ifndef INC_DATALINK_LAYER_DATALINK_DEFINES_H_
#define INC_DATALINK_LAYER_DATALINK_DEFINES_H_


//Message status
#define FAILED -2
#define ACK_TIMED_OUT -1
#define FREE 0
#define ADDED_TO_LIST 1
#define ADDED_TO_BATCH_BUFFER 2
#define PENDING_ACK 3
#define ACKED 4
#define RECEIVED 5

// link status
#define DROPPED -1
#define IDLE 0
#define CONNECTING 1
#define CONNECTED 2
#define DISCONNECTING 3
#define LISTENING 4
#define CONNECTION_RECEIVED 5
#define CONNECTION_ACCEPTED 6
#define NEGOTIATING 7

//connection status
#define IDLE 0
#define TRANSMITTING_DATA 1
#define RECEIVING 2
#define RECEIVING_ACKS_DATA 3
#define ACKNOWLEDGING_DATA 4
#define TRANSMITTING_CONTROL 5
#define RECEIVING_ACKS_CONTROL 6
#define ACKNOWLEDGING_CONTROL 7

//Connection ID
#define BROADCAST_ID 0x00

//Message type
#define NONE 0x00
#define DATA_LONG 0x10
#define DATA_SHORT 0x11
#define ACK_CONTROL 0x20
#define ACK_RANGE 0x21
#define ACK_MULTI 0x22

#define CONTROL 0x30 //multi-commands or especial commands

//Control commands
#define START_CONNECTION 0x31
#define TEST_CONNECTION 0x32
#define CLOSE_CONNECTION 0x33
#define KEEP_ALIVE 0x34
#define FILE_START 0x35
#define FILE_END 0x36
#define PIPE_OPEN 0x37
#define PIPE_CLOSE 0x38
#define SWITCH_ROLE 0x39
#define BLOCK_END 0x3A
#define SET_CONFIG 0x3B
#define REPEAT_LAST_ACK 0x3C

//Error control
#define MESSAGE_ID_ERROR -4
#define MEMORY_ERROR -3
#define MESSAGE_LENGTH_ERROR -2
#define ERROR -1
#define SUCCESSFUL 0

//Node role
#define COMMANDER 0
#define RESPONDER 1

//Gear shift
#define GEAR_SHIFT 255

//Gear shift algorithms
#define SNR_BASED 0
#define SUCCESS_BASED_LADDER 1

//Header length
#define ACK_MULTI_ACK_RANGE_HEADER_LENGTH 3
#define CONTROL_ACK_CONTROL_HEADER_LENGTH 3
#define DATA_LONG_HEADER_LENGTH 4
#define DATA_SHORT_HEADER_LENGTH 5

//Load config level
#define FULL 0
#define PHYSICAL_LAYER_ONLY 1

#define INFINITE -1

#define POLY_CRC8 0xF4

#endif
