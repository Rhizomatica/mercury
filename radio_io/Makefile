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

# Always define UNAME_S.  It used to be assigned only inside the
# HAVE_HERMES_SHM probe below -- a block the top-level Makefile skips, because
# it exports HAVE_HERMES_SHM, so $(origin ...) is "environment" rather than
# "undefined".  UNAME_S was therefore EMPTY on every build driven from the top
# level, and the "else ifeq ($(UNAME_S),Darwin)" arm of the hamlib block could
# never be taken: macOS silently fell through to pkg-config, ignored the
# vendored radio_io/hamlib-macos, and failed with "hamlib/rig.h file not found".
# Building this directory on its own worked, which is what hid it.
UNAME_S := $(shell uname -s)

ifeq ($(origin HAVE_HERMES_SHM), undefined)
ifeq ($(OS),Windows_NT)
HAVE_HERMES_SHM = 0
else
ifeq ($(UNAME_S),Linux)
HAVE_HERMES_SHM = 1
else
HAVE_HERMES_SHM = 0
endif
endif
endif

ifeq ($(HAVE_HAMLIB),1)
ifeq ($(OS),Windows_NT)
HAMLIB_CFLAGS_LOCAL = -Ihamlib-w64/include -DHAVE_HAMLIB
else ifeq ($(UNAME_S),Darwin)
ifneq ($(wildcard hamlib-macos/lib/libhamlib*.a),)
HAMLIB_CFLAGS_LOCAL = -Ihamlib-macos/include -DHAVE_HAMLIB
else
HAMLIB_CFLAGS_LOCAL = $(shell pkg-config --cflags hamlib) -DHAVE_HAMLIB
endif
else
HAMLIB_CFLAGS_LOCAL = $(shell pkg-config --cflags hamlib) -DHAVE_HAMLIB
endif
else
HAMLIB_CFLAGS_LOCAL =
endif

ifeq ($(HAVE_HERMES_SHM),1)
HERMES_SHM_CFLAGS_LOCAL = -DHAVE_HERMES_SHM
endif

CFLAGS = $(COMMON_CFLAGS) $(HAMLIB_CFLAGS_LOCAL) $(HERMES_SHM_CFLAGS_LOCAL)

OBJS = radio_io.o serial_ptt.o cm108_ptt.o

ifeq ($(HAVE_HERMES_SHM),1)
OBJS += sbitx_io.o shm_utils.o
endif

ifeq ($(HAVE_HAMLIB),1)
OBJS += rigctl_parse.o
endif

all: $(OBJS)

radio_io.o: radio_io.c radio_io.h serial_ptt.h cm108_ptt.h sbitx_io.h radio_cmds.h shm_utils.h rigctl_parse.h
	$(CC) $(CFLAGS) -c radio_io.c

serial_ptt.o: serial_ptt.c serial_ptt.h radio_io.h
	$(CC) $(CFLAGS) -c serial_ptt.c

cm108_ptt.o: cm108_ptt.c cm108_ptt.h radio_io.h
	$(CC) $(CFLAGS) -c cm108_ptt.c

sbitx_io.o: sbitx_io.c sbitx_io.h radio_cmds.h
	$(CC) $(CFLAGS) -c sbitx_io.c

shm_utils.o: shm_utils.c shm_utils.h
	$(CC) $(CFLAGS) -c shm_utils.c

rigctl_parse.o: rigctl_parse.c rigctl_parse.h
	$(CC) $(CFLAGS) -c rigctl_parse.c

clean:
	rm -f *.o
