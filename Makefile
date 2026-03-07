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


HAMLIB_W64_DIR = radio_io/hamlib-w64

HAVE_HERMES_SHM = 0

ifeq ($(OS),Windows_NT)
	FFAUDIO_LINKFLAGS += -lole32
	FFAUDIO_LINKFLAGS += -ldsound -ldxguid
	FFAUDIO_LINKFLAGS += -lws2_32
	FFAUDIO_LINKFLAGS += -static-libgcc -static-libstdc++ -l:libwinpthread.a
	HAVE_HAMLIB = 1
	HAMLIB_CFLAGS = -I$(HAMLIB_W64_DIR)/include -DHAVE_HAMLIB
	HAMLIB_LDFLAGS = -L$(HAMLIB_W64_DIR)/lib -lhamlib
else
    UNAME_S := $(shell uname -s)
    ifeq ($(UNAME_S),Linux)
	FFAUDIO_LINKFLAGS += -lpulse
	FFAUDIO_LINKFLAGS += -lasound -lpthread -lrt
	HAVE_HERMES_SHM = 1
    endif
    ifeq ($(UNAME_S),Darwin)
	FFAUDIO_LINKFLAGS := -framework CoreFoundation -framework CoreAudio
    endif
    ifeq ($(UNAME_S),FreeBSD)
	FFAUDIO_LINKFLAGS := -lm
    endif
    HAVE_HAMLIB := $(shell pkg-config --exists hamlib 2>/dev/null && echo 1)
    ifeq ($(HAVE_HAMLIB),1)
	HAMLIB_CFLAGS := $(shell pkg-config --cflags hamlib) -DHAVE_HAMLIB
	HAMLIB_LDFLAGS := $(shell pkg-config --libs hamlib)
    else
	HAMLIB_CFLAGS =
	HAMLIB_LDFLAGS =
    endif
endif

export HAVE_HAMLIB
export HAVE_HERMES_SHM

include config.mk

MINGW_CC  = x86_64-w64-mingw32-gcc
MINGW_AR  = x86_64-w64-mingw32-ar

.PHONY: all install internal_deps utils clean doxygen doxygen-clean windows windows-zip FORCE

prefix ?= /usr
bindir ?= $(prefix)/bin

DOXYGEN ?= doxygen
DOXYFILE ?= Doxyfile

ifeq ($(HAVE_HERMES_SHM),1)
HERMES_SHM_CFLAGS = -DHAVE_HERMES_SHM
endif

CFLAGS = $(COMMON_CFLAGS) -Imodem/freedv -Imodem -Idatalink_broadcast -Idata_interfaces -Idatalink_arq -Iaudioio/ffaudio -Icommon -Igui_interface -Iradio_io $(HAMLIB_CFLAGS) $(HERMES_SHM_CFLAGS)

ifeq ($(OS),Windows_NT)
BINARY = mercury.exe
else
BINARY = mercury
endif

LDFLAGS=$(FFAUDIO_LINKFLAGS) -lm $(HAMLIB_LDFLAGS)

MERCURY_LINK_INPUTS = \
	main.o datalink_arq/arq.o datalink_arq/fsm.o datalink_arq/arith.o datalink_arq/arq_channels.o \
	datalink_arq/arq_fsm.o datalink_arq/arq_protocol.o datalink_arq/arq_timing.o datalink_arq/arq_modem.o \
	datalink_broadcast/broadcast.o datalink_broadcast/kiss.o modem/modem.o modem/framer.o modem/freedv/libfreedvdata.a \
	audioio/audioio.a common/os_interop.o common/ring_buffer_posix.o common/shm_posix.o common/crc6.o common/hermes_log.o \
	common/chan.o common/queue.o data_interfaces/tcp_interfaces.o data_interfaces/net.o \
	gui_interface/ui_communication.o gui_interface/spectrum_sender.o radio_io/radio_io.o

ifeq ($(HAVE_HERMES_SHM),1)
MERCURY_LINK_INPUTS += radio_io/sbitx_io.o radio_io/shm_utils.o
endif

ifeq ($(HAVE_HAMLIB),1)
MERCURY_LINK_INPUTS += radio_io/rigctl_parse.o
endif

all: internal_deps utils
ifeq ($(HAVE_HAMLIB),1)
	@echo "HAMLIB support: enabled"
else
	@echo "HAMLIB support: disabled (install libhamlib-dev and pkg-config to enable)"
endif
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
	$(MAKE) -C gui_interface gui_interface
	$(MAKE) -C radio_io

windows:
	$(MAKE) clean OS=Windows_NT CC=$(MINGW_CC) AR=$(MINGW_AR)
	$(MAKE) -j$$(nproc) OS=Windows_NT CC=$(MINGW_CC) AR=$(MINGW_AR)

WINDOWS_ZIP = mercury-w64-$(GIT_HASH).zip

windows-zip: windows
	rm -rf mercury-w64 $(WINDOWS_ZIP)
	mkdir -p mercury-w64
	cp mercury.exe mercury-w64/
	cp $(HAMLIB_W64_DIR)/bin/*.dll mercury-w64/
	cd mercury-w64 && zip -9 ../$(WINDOWS_ZIP) *
	rm -rf mercury-w64
	@echo "Created $(WINDOWS_ZIP)"

clean:
	rm -f mercury mercury.exe *.o .git_hash_stamp mercury-w64-*.zip
	rm -rf mercury-w64
	$(MAKE) -C modem clean
	$(MAKE) -C datalink_arq clean
	$(MAKE) -C datalink_broadcast clean
	$(MAKE) -C data_interfaces clean
	$(MAKE) -C audioio clean
	$(MAKE) -C common clean
	$(MAKE) -C gui_interface clean
	$(MAKE) -C radio_io clean

doxygen:
	@command -v $(DOXYGEN) >/dev/null 2>&1 || { echo "ERROR: doxygen not found"; exit 1; }
	mkdir -p docs
	$(DOXYGEN) $(DOXYFILE)

doxygen-clean:
	rm -rf docs
