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


ifeq ($(OS),Windows_NT)
	FFAUDIO_LINKFLAGS += -lole32
	FFAUDIO_LINKFLAGS += -ldsound -ldxguid
	FFAUDIO_LINKFLAGS += -lws2_32
	FFAUDIO_LINKFLAGS += -static-libgcc -static-libstdc++ -static -l:libwinpthread.a
else
    UNAME_S := $(shell uname -s)
    ifeq ($(UNAME_S),Linux)
	FFAUDIO_LINKFLAGS += -lpulse
	FFAUDIO_LINKFLAGS += -lasound -lpthread -lrt
    endif
    ifeq ($(UNAME_S),Darwin)
	FFAUDIO_LINKFLAGS := -framework CoreFoundation -framework CoreAudio
    endif
    ifeq ($(UNAME_S),FreeBSD)
	FFAUDIO_LINKFLAGS := -lm
    endif
endif

include config.mk

MINGW_CC  = x86_64-w64-mingw32-gcc
MINGW_AR  = x86_64-w64-mingw32-ar

.PHONY: all install internal_deps utils clean doxygen doxygen-clean windows FORCE

prefix ?= /usr
bindir ?= $(prefix)/bin

DOXYGEN ?= doxygen
DOXYFILE ?= Doxyfile

CFLAGS = $(COMMON_CFLAGS) -Imodem/freedv -Imodem -Idatalink_broadcast -Idata_interfaces -Idatalink_arq -Iaudioio/ffaudio -Icommon -Iradio_io

ifeq ($(OS),Windows_NT)
BINARY = mercury.exe
else
BINARY = mercury
endif

LDFLAGS=$(FFAUDIO_LINKFLAGS) -lm -lhamlib

MERCURY_LINK_INPUTS = \
	main.o datalink_arq/arq.o datalink_arq/fsm.o datalink_arq/arith.o datalink_arq/arq_channels.o \
	datalink_arq/arq_fsm.o datalink_arq/arq_protocol.o datalink_arq/arq_timing.o datalink_arq/arq_modem.o \
	datalink_broadcast/broadcast.o datalink_broadcast/kiss.o modem/modem.o modem/framer.o modem/freedv/libfreedvdata.a \
	audioio/audioio.a common/os_interop.o common/ring_buffer_posix.o common/shm_posix.o common/crc6.o common/hermes_log.o \
	common/chan.o common/queue.o data_interfaces/tcp_interfaces.o data_interfaces/net.o \
	radio_io/radio_io.o radio_io/sbitx_io.o radio_io/shm_utils.o radio_io/rigctl_parse.o

all: internal_deps utils
	$(MAKE) $(BINARY)
	$(MAKE) -C utils

install: all
	install -D -m 755 $(BINARY) $(DESTDIR)$(bindir)/mercury

$(BINARY): $(MERCURY_LINK_INPUTS)
	$(CC) -o $(BINARY)  \
		$(MERCURY_LINK_INPUTS) $(LDFLAGS)

# Stamp file: written only when GIT_HASH changes so main.o is rebuilt
# exactly when needed (FORCE makes the recipe always run; the recipe
# only touches the file when the content actually differs).
.git_hash_stamp: FORCE
	@if [ ! -f $@ ] || [ "$$(cat $@)" != "$(GIT_HASH)" ]; then \
		printf '%s' "$(GIT_HASH)" > $@; \
	fi

FORCE:

main.o: main.c .git_hash_stamp
	$(CC) $(CFLAGS) -c main.c

internal_deps:
	$(MAKE) -C modem
	$(MAKE) -C datalink_arq
	$(MAKE) -C datalink_broadcast
	$(MAKE) -C data_interfaces
	$(MAKE) -C audioio
	$(MAKE) -C common
	$(MAKE) -C radio_io


windows:
	$(MAKE) clean OS=Windows_NT CC=$(MINGW_CC) AR=$(MINGW_AR)
	$(MAKE) -j$$(nproc) OS=Windows_NT CC=$(MINGW_CC) AR=$(MINGW_AR)

clean:
	rm -f mercury mercury.exe *.o .git_hash_stamp
	$(MAKE) -C modem clean
	$(MAKE) -C datalink_arq clean
	$(MAKE) -C datalink_broadcast clean
	$(MAKE) -C data_interfaces clean
	$(MAKE) -C audioio clean
	$(MAKE) -C common clean
	$(MAKE) -C radio_io clean

doxygen:
	@command -v $(DOXYGEN) >/dev/null 2>&1 || { echo "ERROR: doxygen not found"; exit 1; }
	mkdir -p docs
	$(DOXYGEN) $(DOXYFILE)

doxygen-clean:
	rm -rf docs
