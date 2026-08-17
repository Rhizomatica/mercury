# Makefile for WebSocket-based UI communication
#
# Copyright (C) 2025 Rhizomatica
# Author: Rafael Diniz <rafael@riseup.net>
#
# This is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 3, or (at your option)
# any later version.
#
# This software is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this software; see the file COPYING.  If not, write to
# the Free Software Foundation, Inc., 51 Franklin Street,
# Boston, MA 02110-1301, USA.
#

include ../config.mk

CFLAGS_COMMON = $(COMMON_CFLAGS) -Wno-stringop-truncation -I../datalink_arq -I../data_interfaces -I../common -I../modem/freedv

# Mongoose / WebSocket flags
# Use mongoose built-in TLS backend.
WS_CFLAGS = $(COMMON_CFLAGS) -Iwebsocket -DMG_TLS=MG_TLS_BUILTIN

# Objects
OBJS = ui_communication.o ui_status.o ui_devices.o
WS_OBJS = websocket/mongoose.o websocket/mercury_websocket.o

all: $(OBJS) $(WS_OBJS)

ui_communication.o: ui_communication.c ui_communication.h ui_status.h
	$(CC) $(CFLAGS_COMMON) -c ui_communication.c -o ui_communication.o

ui_devices.o: ui_devices.c ui_communication.h ui_status.h
	$(CC) $(CFLAGS_COMMON) -c ui_devices.c -o ui_devices.o

ui_status.o: ui_status.c ui_status.h
	$(CC) $(CFLAGS_COMMON) -c ui_status.c -o ui_status.o

websocket/mongoose.o: websocket/mongoose.c websocket/mongoose.h
	$(CC) $(WS_CFLAGS) -Wno-maybe-uninitialized -c websocket/mongoose.c -o websocket/mongoose.o

websocket/mercury_websocket.o: websocket/mercury_websocket.c websocket/mercury_websocket.h
	$(CC) $(WS_CFLAGS) -c websocket/mercury_websocket.c -o websocket/mercury_websocket.o

clean:
	rm -f $(OBJS) $(WS_OBJS)

.PHONY: all clean
