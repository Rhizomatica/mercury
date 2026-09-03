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


# The vendored RaptorQ tree builds into its own subdirectories, so a flat
# `rm -f *.o` leaves those objects behind.  That is not cosmetic: the universal
# macOS build runs `make clean` between its x86_64 and arm64 slices, so stale
# objects from the first slice survive into the second and `ar` then refuses
# the archive -- "cputype (16777223) does not match previous archive members
# cputype (16777228)".  Same trap for a Windows cross build after a native one,
# which is why those objects are named .w64.o; clean them here too.
clean:
	rm -f *.o *.w64.o *.d
	rm -f raptorq/lib/*.o raptorq/lib/*.w64.o raptorq/lib/*.d
	rm -f raptorq/deps/obl/*.o raptorq/deps/obl/*.w64.o raptorq/deps/obl/*.d
