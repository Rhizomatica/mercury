# HERMES Modem - Radio I/O
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

CFLAGS = $(COMMON_CFLAGS)

all: radio_io.o sbitx_io.o shm_utils.o rigctl_parse.o

radio_io.o: radio_io.c radio_io.h sbitx_io.h radio_cmds.h shm_utils.h rigctl_parse.h
	$(CC) $(CFLAGS) -c radio_io.c

sbitx_io.o: sbitx_io.c sbitx_io.h radio_cmds.h
	$(CC) $(CFLAGS) -c sbitx_io.c

shm_utils.o: shm_utils.c shm_utils.h
	$(CC) $(CFLAGS) -c shm_utils.c

rigctl_parse.o: rigctl_parse.c rigctl_parse.h
	$(CC) $(CFLAGS) -c rigctl_parse.c

clean:
	rm -f *.o
