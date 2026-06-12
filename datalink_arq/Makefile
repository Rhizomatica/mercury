# HERMES Modem
#
# Copyright (C) 2024-2025 Rhizomatica
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

CFLAGS = $(COMMON_CFLAGS) -I../modem/freedv -I../modem -I../datalink_broadcast -I.. -I../audioio/ffaudio -I../common -I../data_interfaces

LDFLAGS=$(FFAUDIO_LINKFLAGS) -lm

all: arq.o arith.o arq_channels.o arq_fsm.o arq_protocol.o arq_timing.o arq_modem.o

arq.o: arq.c
	$(CC) $(CFLAGS) -c arq.c


arith.o: arith.c
	$(CC) $(CFLAGS) -c arith.c

arq_channels.o: arq_channels.c arq_channels.h arq_events.h
	$(CC) $(CFLAGS) -c arq_channels.c

arq_fsm.o: arq_fsm.c arq_fsm.h arq.h
	$(CC) $(CFLAGS) -c arq_fsm.c

arq_protocol.o: arq_protocol.c arq_protocol.h
	$(CC) $(CFLAGS) -c arq_protocol.c

arq_timing.o: arq_timing.c arq_timing.h arq_protocol.h
	$(CC) $(CFLAGS) -c arq_timing.c

arq_modem.o: arq_modem.c arq_modem.h arq_fsm.h arq.h
	$(CC) $(CFLAGS) -c arq_modem.c

clean:
	rm -f *.o
