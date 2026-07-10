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
HAMLIB_W64_LIBS = $(wildcard $(HAMLIB_W64_DIR)/lib/libhamlib*.a)

HAVE_HERMES_SHM = 0

ifeq ($(OS),Windows_NT)
	FFAUDIO_LINKFLAGS += -lole32
	FFAUDIO_LINKFLAGS += -ldsound -ldxguid
	FFAUDIO_LINKFLAGS += -lws2_32
	FFAUDIO_LINKFLAGS += -static-libgcc -static-libstdc++ -l:libwinpthread.a
	ifneq ($(strip $(HAMLIB_W64_LIBS)),)
		HAVE_HAMLIB = 1
		HAMLIB_CFLAGS = -I$(HAMLIB_W64_DIR)/include -DHAVE_HAMLIB
		HAMLIB_LDFLAGS = -L$(HAMLIB_W64_DIR)/lib -lhamlib
	else
		HAVE_HAMLIB = 0
		HAMLIB_CFLAGS =
		HAMLIB_LDFLAGS =
	endif
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

FYNE_UI_DIR   = gui_interface/fyne-ui
FYNE_UI_BIN   = mercury-ui.exe
MINGW_GO_CC   = x86_64-w64-mingw32-gcc

.PHONY: all install internal_deps utils clean doxygen doxygen-clean windows windows-zip fyne-ui fyne-ui-windows windows-installer test integration-test FORCE

prefix ?= /usr
bindir ?= $(prefix)/bin
mandir ?= $(prefix)/share/man

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
	main.o common/cfg_utils.o common/iniparser/iniparser.o common/iniparser/dictionary.o \
	datalink_arq/arq.o datalink_arq/arq_tnc.o datalink_arq/arith.o datalink_arq/arq_channels.o \
	datalink_arq/arq_fsm.o datalink_arq/arq_protocol.o datalink_arq/arq_timing.o datalink_arq/arq_modem.o \
	datalink_broadcast/broadcast.o datalink_broadcast/kiss.o modem/modem.o modem/framer.o modem/channel_busy.o modem/freedv/libfreedvdata.a \
	audioio/audioio.a common/os_interop.o common/ring_buffer_posix.o common/shm_posix.o common/crc6.o common/hermes_log.o \
	common/chan.o common/queue.o data_interfaces/tcp_interfaces.o data_interfaces/net.o \
	gui_interface/ui_communication.o \
	gui_interface/websocket/mongoose.o gui_interface/websocket/mercury_websocket.o \
	radio_io/radio_io.o

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

docdir ?= $(prefix)/share/doc/mercury

install: all
	install -D -m 755 $(BINARY) $(DESTDIR)$(bindir)/mercury
	install -D -m 644 mercury.1 $(DESTDIR)$(mandir)/man1/mercury.1
	install -D -m 644 mercury.ini.example $(DESTDIR)$(docdir)/mercury.ini.example

$(BINARY): $(MERCURY_LINK_INPUTS)
	$(CC) -o $(BINARY)  \
		$(MERCURY_LINK_INPUTS) $(LDFLAGS) $(SAN_LDFLAGS)

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
	$(MAKE) -C gui_interface
	$(MAKE) -C radio_io

windows:
	$(MAKE) clean OS=Windows_NT CC=$(MINGW_CC) AR=$(MINGW_AR)
	$(MAKE) -j$$(nproc) OS=Windows_NT CC=$(MINGW_CC) AR=$(MINGW_AR)

MERCURY_VERSION ?= $(shell grep 'define VERSION__' main.c | head -1 | sed 's/.*"\(.*\)".*/\1/')
WINDOWS_DIR = mercury-$(MERCURY_VERSION)
WINDOWS_ZIP = $(WINDOWS_DIR)-w64-$(GIT_HASH).zip

MERCURY_CORE_OBJS = \
	common/cfg_utils.o common/iniparser/iniparser.o common/iniparser/dictionary.o \
	datalink_arq/arq.o datalink_arq/arq_tnc.o datalink_arq/arith.o datalink_arq/arq_channels.o \
	datalink_arq/arq_fsm.o datalink_arq/arq_protocol.o datalink_arq/arq_timing.o datalink_arq/arq_modem.o \
	datalink_broadcast/broadcast.o datalink_broadcast/kiss.o \
	modem/modem.o modem/framer.o modem/channel_busy.o \
	common/os_interop.o common/ring_buffer_posix.o common/shm_posix.o common/crc6.o common/hermes_log.o \
	common/chan.o common/queue.o \
	data_interfaces/tcp_interfaces.o data_interfaces/net.o \
	gui_interface/ui_communication.o \
	gui_interface/websocket/mongoose.o gui_interface/websocket/mercury_websocket.o \
	radio_io/radio_io.o

ifeq ($(HAVE_HERMES_SHM),1)
MERCURY_CORE_OBJS += radio_io/sbitx_io.o radio_io/shm_utils.o
endif

ifeq ($(HAVE_HAMLIB),1)
MERCURY_CORE_OBJS += radio_io/rigctl_parse.o
endif

MERCURY_CORE_OBJS_W64 = $(filter-out radio_io/sbitx_io.o radio_io/shm_utils.o,$(MERCURY_CORE_OBJS))

libmercury_core.a: internal_deps
	$(CC) $(CFLAGS) -I. -c $(FYNE_UI_DIR)/engine/mercury_bridge.c -o $(FYNE_UI_DIR)/engine/mercury_bridge.o
	$(AR) rcs $@ $(MERCURY_CORE_OBJS) $(FYNE_UI_DIR)/engine/mercury_bridge.o

libmercury_core_w64.a:
	$(MAKE) clean OS=Windows_NT CC=$(MINGW_CC) AR=$(MINGW_AR)
	$(MAKE) internal_deps OS=Windows_NT CC=$(MINGW_CC) AR=$(MINGW_AR) HAVE_HERMES_SHM=0
	$(MINGW_CC) $(CFLAGS) -I. -c $(FYNE_UI_DIR)/engine/mercury_bridge.c -o $(FYNE_UI_DIR)/engine/mercury_bridge_w64.o
	$(MINGW_AR) rcs $@ $(MERCURY_CORE_OBJS_W64) $(FYNE_UI_DIR)/engine/mercury_bridge_w64.o

fyne-ui: libmercury_core.a
	@echo "Building Mercury UI for Linux..."
	cd $(FYNE_UI_DIR) && CGO_ENABLED=1 go build -tags mercury_embedded -o mercury-ui .
	@echo "  -> $(FYNE_UI_DIR)/mercury-ui"

windows-zip: windows fyne-ui-windows
	rm -rf $(WINDOWS_DIR) $(WINDOWS_ZIP)
	mkdir -p $(WINDOWS_DIR)
	cp mercury.exe $(WINDOWS_DIR)/
	cp $(WINDOWS_INSTALLER_DIR)/$(FYNE_UI_BIN) $(WINDOWS_DIR)/
	cp mercury.ini.example $(WINDOWS_DIR)/
	if ls $(HAMLIB_W64_DIR)/bin/*.dll >/dev/null 2>&1; then \
		cp $(HAMLIB_W64_DIR)/bin/*.dll $(WINDOWS_DIR)/; \
	fi
	zip -9r $(WINDOWS_ZIP) $(WINDOWS_DIR)
	rm -rf $(WINDOWS_DIR)
	@echo "Created $(WINDOWS_ZIP)"

WINDOWS_INSTALLER_DIR = windows-installer

fyne-ui-windows: libmercury_core_w64.a
	@echo "Building single-binary Mercury UI for Windows..."
	cd $(FYNE_UI_DIR) && \
		CGO_ENABLED=1 GOOS=windows GOARCH=amd64 CC=$(MINGW_GO_CC) \
		go build -tags mercury_embedded -ldflags="-s -w -H windowsgui" -o $(abspath $(WINDOWS_INSTALLER_DIR)/$(FYNE_UI_BIN)) .
	@echo "  -> $(WINDOWS_INSTALLER_DIR)/$(FYNE_UI_BIN)"

windows-installer: fyne-ui-windows
	cp mercury.ini.example $(WINDOWS_INSTALLER_DIR)/mercury.ini
	sed -i 's/ui_enabled = false/ui_enabled = true/g' $(WINDOWS_INSTALLER_DIR)/mercury.ini
	sed -i 's/sound_system = auto/sound_system = dsound/g' $(WINDOWS_INSTALLER_DIR)/mercury.ini
	if ls $(HAMLIB_W64_DIR)/bin/*.dll >/dev/null 2>&1; then \
		cp $(HAMLIB_W64_DIR)/bin/*.dll $(WINDOWS_INSTALLER_DIR)/; \
	fi
	@echo "windows-installer ready: run Inno Setup on $(WINDOWS_INSTALLER_DIR)/installer.iss"

clean:
	rm -f mercury mercury.exe *.o .git_hash_stamp mercury-*.zip libmercury_core.a
	rm -rf mercury-[0-9]*
	rm -f $(WINDOWS_INSTALLER_DIR)/$(FYNE_UI_BIN)
	rm -f $(FYNE_UI_DIR)/engine/mercury_bridge.o $(FYNE_UI_DIR)/engine/mercury_bridge_w64.o
	rm -f $(FYNE_UI_DIR)/mercury-ui
	$(MAKE) -C modem clean
	$(MAKE) -C datalink_arq clean
	$(MAKE) -C datalink_broadcast clean
	$(MAKE) -C data_interfaces clean
	$(MAKE) -C audioio clean
	$(MAKE) -C common clean
	$(MAKE) -C gui_interface clean
	$(MAKE) -C radio_io clean
	$(MAKE) -C utils clean

doxygen:
	@command -v $(DOXYGEN) >/dev/null 2>&1 || { echo "ERROR: doxygen not found"; exit 1; }
	mkdir -p docs
	$(DOXYGEN) $(DOXYFILE)

doxygen-clean:
	rm -rf docs

test:
	$(MAKE) -C tests test

integration-test:
	cd tests/integration && go test -v ./...
