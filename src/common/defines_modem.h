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

#ifndef HAVE_DEFINES_MODEM_H
#define HAVE_DEFINES_MODEM_H

#ifndef MAX_PATH
#define MAX_PATH 255
#endif

// {TX,RX}_SHM broadcast memory interface
#define SHM_PAYLOAD_BUFFER_SIZE 131072
#define SHM_PAYLOAD_NAME "/broadcast"

#define INT_BUFFER_SIZE 4096

#define DATA_TX_BUFFER_SIZE 8192
#define DATA_RX_BUFFER_SIZE 8192

// audio buffers shared memory interface
// 1536000 * 8
#define SIGNAL_BUFFER_SIZE 12288000
#define SIGNAL_INPUT "/signal-radio2modem"
#define SIGNAL_OUTPUT "/signal-modem2radio"

#if defined(_WIN32)
#define msleep(a) Sleep(a)
#else
#define msleep(a) usleep(a * 1000)
#endif


#endif // HAVE_DEFINES_MODEM_H
