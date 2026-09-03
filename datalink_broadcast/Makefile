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

CFLAGS = $(COMMON_CFLAGS) -I../modem -I../modem/freedv -I../common -I../datalink_arq -I../data_interfaces
all: broadcast.o kiss.o

kiss.o: kiss.c
	$(CC) $(CFLAGS) -c kiss.c

broadcast.o: broadcast.c
	$(CC) $(CFLAGS) -c broadcast.c


clean:
	rm -f *.o

# Pull in the header dependencies emitted by -MMD (see config.mk).  Leading
# dash: absent on a first build, which is correct.
-include $(wildcard *.d */*.d */*/*.d)
