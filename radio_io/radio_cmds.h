/* Rhizomatica HERMES transceiver serial commands
 * Copyright (C) 2021-2024 Rhizomatica
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
 * along with this software; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street,
 * Boston, MA 02110-1301, USA.
 *
 */

#ifndef HAVE_CMDS_H__
#define HAVE_CMDS_H__

// we need a 8bit clean connection
// We don't care about endianess (little endian assumed)
// IT MIGHT NOT WORK ON BIG ENDIAN MACHINES RIGHT NOW!

// ================================= //
// per profile (2 upper bits for profile, 6 bits for the command itself), so commands only from 0 to 0x3F only!
#define CMD_GET_FREQ 0x01
#define CMD_SET_FREQ 0x02

#define CMD_GET_MODE 0x03
#define CMD_SET_MODE 0x04

#define CMD_SET_VOLUME 0x05
#define CMD_GET_VOLUME 0x06

#define CMD_SET_AGC 0x07
#define CMD_GET_AGC 0x08

#define CMD_SET_COMPRESSOR 0x09
#define CMD_GET_COMPRESSOR 0x0a

// pack this in bit fields inside 32bits
#define CMD_SET_KNOBS 0x0b
#define CMD_GET_KNOBS 0x0c

// lower 16bit word for BPF_SLOW and upper 16 bit for BPF_HIGH
#define CMD_SET_LPF 0x0d
#define CMD_GET_LPF 0x0f

#define CMD_PTT_ON 0x10
#define CMD_PTT_OFF 0x11

#define CMD_GET_LED_STATUS 0x12
#define CMD_SET_LED_STATUS 0x13

#define CMD_GET_CONNECTED_STATUS 0x14
#define CMD_SET_CONNECTED_STATUS 0x15

// now the general commands (not per profile)
#define CMD_GET_PROFILE 0x16
#define CMD_SET_PROFILE 0x17

#define CMD_GET_PROTECTION_STATUS 0x18
#define CMD_RESET_PROTECTION 0x19

#define CMD_GET_TXRX_STATUS 0x1a

#define CMD_GET_BFO 0x1b
#define CMD_SET_BFO 0x1c

#define CMD_GET_FWD 0x1d
#define CMD_GET_REF 0x1f

#define CMD_SET_SERIAL 0x20
#define CMD_GET_SERIAL 0x21

#define CMD_SET_REF_THRESHOLD 0x22
#define CMD_GET_REF_THRESHOLD 0x23

#define CMD_SET_RADIO_DEFAULTS 0x24
#define CMD_RESTORE_RADIO_DEFAULTS 0x25

#define CMD_RADIO_RESET 0x26

#define CMD_SET_STEPHZ 0x27
#define CMD_GET_STEPHZ 0x28

#define CMD_SET_TONE 0x29
#define CMD_GET_TONE 0x2a

// legacy stuff
#define CMD_GET_MASTERCAL 0x2b
#define CMD_SET_MASTERCAL 0x2c
#define CMD_GPS_CALIBRATE 0x2d
#define CMD_GET_STATUS 0x2e
// legacy stuff/

#define CMD_TIMEOUT_RESET 0x2f

#define CMD_SET_TIMEOUT 0x30
#define CMD_GET_TIMEOUT 0x31

#define CMD_SET_BITRATE 0x32
#define CMD_GET_BITRATE 0x33

#define CMD_SET_SNR 0x34
#define CMD_GET_SNR 0x35

#define CMD_SET_BYTES_RX 0x36
#define CMD_GET_BYTES_RX 0x37

#define CMD_SET_BYTES_TX 0x38
#define CMD_GET_BYTES_TX 0x39

#define CMD_SET_POWER 0x3a
#define CMD_GET_POWER 0x3b

#define CMD_GET_DIGITAL_VOICE 0x3c
#define CMD_SET_DIGITAL_VOICE 0x3d

// ================================= //


// ================================= //
// radio responses
#define CMD_RESP_TIMEOUT 0x00
#define CMD_RESP_GET_FREQ_ACK 0x01
#define CMD_RESP_GET_BFO_ACK 0x02
#define CMD_RESP_GET_FWD_ACK 0x03
#define CMD_RESP_GET_REF_ACK 0x04
#define CMD_RESP_GET_SERIAL_ACK 0x05
#define CMD_RESP_GET_REF_THRESHOLD_ACK 0x06
#define CMD_RESP_GET_STEPHZ_ACK 0x07
#define CMD_RESP_GET_VOLUME_ACK 0x08
#define CMD_RESP_GET_TONE_ACK 0x09
#define CMD_RESP_GET_AGC 0x0a
#define CMD_RESP_GET_COMPRESSOR 0x0b
#define CMD_RESP_GET_KNOBS 0x0c
#define CMD_RESP_GET_LPF 0x0d
#define CMD_RESP_GET_PROFILE 0x0f
#define CMD_RESP_GET_POWER 0x10
#define CMD_ALERT_PROTECTION_ON 0x11

// legacy stuff
#define CMD_RESP_GET_STATUS_ACK 0x12
#define CMD_RESP_GPS_NOT_PRESENT 0x13
#define CMD_RESP_GET_MASTERCAL_ACK 0x14
// legacy stuff/

#define CMD_RESP_ACK 0x15 // general ACK
#define CMD_RESP_PTT_ON_NACK 0x16
#define CMD_RESP_PTT_OFF_NACK 0x17

#define CMD_RESP_GET_MODE_USB 0x18
#define CMD_RESP_GET_MODE_LSB 0x19
#define CMD_RESP_GET_MODE_CW 0x1a

#define CMD_RESP_GET_TXRX_INTX 0x1b
#define CMD_RESP_GET_TXRX_INRX 0x1c

#define CMD_RESP_GET_PROTECTION_ON 0x1d
#define CMD_RESP_GET_PROTECTION_OFF 0x1e

#define CMD_RESP_GET_LED_STATUS_ON 0x1f
#define CMD_RESP_GET_LED_STATUS_OFF 0x20

#define CMD_RESP_GET_CONNECTED_STATUS_ON 0x21
#define CMD_RESP_GET_CONNECTED_STATUS_OFF 0x22

#define CMD_RESP_GET_TIMEOUT_ACK 0x23

#define CMD_RESP_GET_BITRATE 0x24
#define CMD_RESP_GET_SNR 0x25

#define CMD_RESP_GET_BYTES_RX 0x26
#define CMD_RESP_GET_BYTES_TX 0x27

#define CMD_RESP_WRONG_COMMAND 0x28

#define CMD_RESP_GET_DIGITAL_VOICE_ON 0x29
#define CMD_RESP_GET_DIGITAL_VOICE_OFF 0x2a

#endif // HAVE_CMDS_H__
