# Makefile for UDP JSON UI communication
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
LDFLAGS = -pthread

# Objects
OBJS = ui_communication.o spectrum_sender.o

# Standalone test binary
TARGET = ui_communication

all: $(TARGET)

$(TARGET): ui_communication.c ui_communication.h spectrum_sender.c spectrum_sender.h
	$(CC) $(CFLAGS_COMMON) -DTEST_MAIN -c ui_communication.c -o ui_communication_test.o
	$(CC) $(CFLAGS_COMMON) -c spectrum_sender.c -o spectrum_sender.o
	$(CC) $(CFLAGS_COMMON) -o $@ ui_communication_test.o spectrum_sender.o $(LDFLAGS)

# Library object (no TEST_MAIN) — used by top-level mercury build
gui_interface: ui_communication.c ui_communication.h spectrum_sender.c spectrum_sender.h
	$(CC) $(CFLAGS_COMMON) -c ui_communication.c -o ui_communication.o
	$(CC) $(CFLAGS_COMMON) -c spectrum_sender.c -o spectrum_sender.o

clean:
	rm -f $(OBJS) $(TARGET) ui_communication_test.o

.PHONY: all gui_interface clean
