/* KISS framer
 *
 * Copyright (C) 2020-2024 Rhizomatica
 * Author: Rafael Diniz <rafael@rhizomatica.org>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 */

#include <unistd.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include "kiss.h"

int frame_len;
bool IN_FRAME;
bool ESCAPE;

uint8_t kiss_command = CMD_UNKNOWN;

static bool kiss_cmd_carries_data(uint8_t cmd)
{
    return cmd == CMD_DATA || cmd == CMD_AX25 || cmd == CMD_AX25CALLSIGN;
}

void kiss_reset_state(void)
{
    frame_len = 0;
    IN_FRAME = false;
    ESCAPE = false;
    kiss_command = CMD_UNKNOWN;
}

uint8_t kiss_last_command(void)
{
    return kiss_command;
}

int kiss_read(uint8_t sbyte, uint8_t *frame_buffer)
{
    if (IN_FRAME && sbyte == FEND && kiss_cmd_carries_data(kiss_command))
    {
        IN_FRAME = false;
        return frame_len;
    }
    if (sbyte == FEND)
    {
        IN_FRAME = true;
        kiss_command = CMD_UNKNOWN;
        frame_len = 0;
        return 0;
    }
    if (IN_FRAME && frame_len < MAX_PAYLOAD)
    {
        // Have a look at the command byte first
        if (frame_len == 0 && kiss_command == CMD_UNKNOWN)
        {
            // Strip of port nibble
            kiss_command = sbyte & 0x0F;
            return 0;
        }
        if (kiss_cmd_carries_data(kiss_command))
        {
            if (sbyte == FESC)
            {
                ESCAPE = true;
                return 0;
            }
            if (ESCAPE)
            {
                if (sbyte == TFEND) sbyte = FEND;
                if (sbyte == TFESC) sbyte = FESC;
                ESCAPE = false;
            }
            if (frame_len < MAX_PAYLOAD)
            {
                frame_buffer[frame_len++] = sbyte;
            }
        }
    }
    return 0;
}

int kiss_write_frame(uint8_t* buffer, int frame_len, uint8_t cmd, uint8_t *write_buffer)
{
    int write_len = 0;
    write_buffer[write_len++] = FEND;
    write_buffer[write_len++] = cmd;
    for (int i = 0; i < frame_len; i++)
    {
        uint8_t byte = buffer[i];
        switch(byte)
        {
        case FEND:
            write_buffer[write_len++] = FESC;
            write_buffer[write_len++] = TFEND;
            break;
        case FESC:
            write_buffer[write_len++] = FESC;
            write_buffer[write_len++] = TFESC;
            break;
        default:
            write_buffer[write_len++] = byte;
        }
    }
    write_buffer[write_len++] = FEND;
    return write_len;
}
