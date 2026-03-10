# HERMES Modem
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

CFLAGS = $(COMMON_CFLAGS) -I../modem -I../modem/freedv -I../common -I../datalink_arq -I../datalink_broadcast -I../third_party/chan -I../radio_io

all: tcp_interfaces.o net.o

tcp_interfaces.o: tcp_interfaces.c tcp_interfaces.h
	$(CC) $(CFLAGS) -c tcp_interfaces.c

net.o: net.c net.h
	$(CC) $(CFLAGS) -c net.c


clean:
	rm -f *.o
