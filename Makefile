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

# Vendored universal (x86_64+arm64) static hamlib+libusb for macOS, mirroring
# hamlib-w64 for Windows — self-contained, no Homebrew dependency.
HAMLIB_MACOS_DIR = radio_io/hamlib-macos
HAMLIB_MACOS_LIBS = $(wildcard $(HAMLIB_MACOS_DIR)/lib/libhamlib*.a)
MACOS_HAMLIB_FRAMEWORKS = -framework IOKit -framework CoreFoundation -framework Security

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
    ifeq ($(UNAME_S),Darwin)
    # macOS: prefer the vendored universal static hamlib+libusb (self-contained,
    # no Homebrew) when present, mirroring the Windows hamlib-w64 vendoring;
    # otherwise fall back to pkg-config / Homebrew.
    ifneq ($(strip $(HAMLIB_MACOS_LIBS)),)
    HAVE_HAMLIB := 1
    HAMLIB_CFLAGS := -I$(HAMLIB_MACOS_DIR)/include -DHAVE_HAMLIB
    HAMLIB_LDFLAGS := $(HAMLIB_MACOS_DIR)/lib/libhamlib.a $(HAMLIB_MACOS_DIR)/lib/libusb-1.0.a $(MACOS_HAMLIB_FRAMEWORKS)
    else
    HAVE_HAMLIB := $(shell pkg-config --exists hamlib 2>/dev/null && echo 1)
    ifeq ($(HAVE_HAMLIB),1)
    HAMLIB_CFLAGS := $(shell pkg-config --cflags hamlib) -DHAVE_HAMLIB
    HAMLIB_LDFLAGS := $(shell pkg-config --libs hamlib)
    endif
    endif
    else
    # Linux / FreeBSD: pkg-config detection (unchanged).
    HAVE_HAMLIB := $(shell pkg-config --exists hamlib 2>/dev/null && echo 1)
    ifeq ($(HAVE_HAMLIB),1)
    HAMLIB_CFLAGS := $(shell pkg-config --cflags hamlib) -DHAVE_HAMLIB
    HAMLIB_LDFLAGS := $(shell pkg-config --libs hamlib)
    else
    HAMLIB_CFLAGS =
    HAMLIB_LDFLAGS =
    endif
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

.PHONY: all install internal_deps utils clean doxygen doxygen-clean windows windows-zip windows-installer-signed fyne-ui fyne-ui-macos fyne-ui-macos-dmg macos-universal fyne-ui-macos-universal fyne-ui-macos-universal-dmg fyne-ui-windows windows-installer test integration-test FORCE

prefix ?= /usr
bindir ?= $(prefix)/bin
mandir ?= $(prefix)/share/man

DOXYGEN ?= doxygen
DOXYFILE ?= Doxyfile

ifeq ($(HAVE_HERMES_SHM),1)
HERMES_SHM_CFLAGS = -DHAVE_HERMES_SHM
endif

CFLAGS = $(COMMON_CFLAGS) -I. -Imodem/freedv -Imodem -Idatalink_broadcast -Idata_interfaces -Idatalink_arq -Iaudioio -Iaudioio/ffaudio -Icommon -Igui_interface -Iradio_io $(HAMLIB_CFLAGS) $(HERMES_SHM_CFLAGS)

ifeq ($(OS),Windows_NT)
BINARY = mercury.exe
else
BINARY = mercury
endif

LDFLAGS=$(FFAUDIO_LINKFLAGS) -lm $(HAMLIB_LDFLAGS) $(ATOMIC_LDFLAGS)

MERCURY_LINK_INPUTS = \
	main.o common/cfg_utils.o common/iniparser/iniparser.o common/iniparser/dictionary.o \
	datalink_arq/arq.o datalink_arq/arq_tnc.o datalink_arq/arith.o datalink_arq/arq_channels.o \
	datalink_arq/arq_fsm.o datalink_arq/arq_protocol.o datalink_arq/arq_timing.o datalink_arq/arq_modem.o \
	datalink_broadcast/broadcast.o datalink_broadcast/kiss.o modem/modem.o modem/framer.o modem/channel_busy.o modem/freedv/libfreedvdata.a \
	audioio/audioio.a common/os_interop.o common/ring_buffer_posix.o common/shm_posix.o common/crc6.o common/hermes_log.o common/virtual_clock.o \
	common/chan.o common/queue.o common/mercury_engine.o common/mercury_cli.o data_interfaces/tcp_interfaces.o data_interfaces/net.o \
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

MERCURY_VERSION ?= $(shell grep 'define MERCURY_VERSION "' common/mercury_version.h | head -1 | sed 's/.*"\(.*\)".*/\1/')
WINDOWS_DIR = mercury-$(MERCURY_VERSION)
WINDOWS_ZIP = $(WINDOWS_DIR)-w64-$(GIT_HASH).zip

MERCURY_CORE_OBJS = \
	common/cfg_utils.o common/iniparser/iniparser.o common/iniparser/dictionary.o \
	datalink_arq/arq.o datalink_arq/arq_tnc.o datalink_arq/arith.o datalink_arq/arq_channels.o \
	datalink_arq/arq_fsm.o datalink_arq/arq_protocol.o datalink_arq/arq_timing.o datalink_arq/arq_modem.o \
	datalink_broadcast/broadcast.o datalink_broadcast/kiss.o \
	modem/modem.o modem/framer.o modem/channel_busy.o \
	common/os_interop.o common/ring_buffer_posix.o common/shm_posix.o common/crc6.o common/hermes_log.o common/virtual_clock.o \
	common/chan.o common/queue.o common/mercury_engine.o common/mercury_cli.o \
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
ifneq ($(strip $(HAMLIB_W64_LIBS)),)
MERCURY_CORE_OBJS_W64 += radio_io/rigctl_parse.o
endif

libmercury_core.a: internal_deps
	$(CC) $(CFLAGS) -I. -c $(FYNE_UI_DIR)/engine/mercury_bridge.c -o $(FYNE_UI_DIR)/engine/mercury_bridge.o
	# Remove a stale archive first: macOS ar (cctools) refuses to update an
	# existing *fat* .a in place, so a leftover universal build would wedge the
	# next native build ("is a fat file"). Fresh create is identical on Linux.
	rm -f $@
	$(AR) rcs $@ $(MERCURY_CORE_OBJS) $(FYNE_UI_DIR)/engine/mercury_bridge.o

libmercury_core_w64.a:
	$(MAKE) internal_deps OS=Windows_NT CC=$(MINGW_CC) AR=$(MINGW_AR) HAVE_HERMES_SHM=0
	$(MINGW_CC) $(CFLAGS) -I. -c $(FYNE_UI_DIR)/engine/mercury_bridge.c -o $(FYNE_UI_DIR)/engine/mercury_bridge_w64.o
	$(MINGW_AR) rcs $@ $(MERCURY_CORE_OBJS_W64) $(FYNE_UI_DIR)/engine/mercury_bridge_w64.o

fyne-ui: libmercury_core.a
	@echo "Building Mercury UI (native: Linux or macOS)..."
	cd $(FYNE_UI_DIR) && CGO_ENABLED=1 go build -tags mercury_embedded \
		-ldflags "-X main.coreBuildID=$$(cksum $(abspath libmercury_core.a) | cut -d' ' -f1)" \
		-o $(abspath mercury-ui) .
	@echo "  -> mercury-ui"

# macOS .app bundle.  Fyne's packaging tool builds the binary (with the
# mercury_embedded tag, so it links via mercury_link_darwin.go) and wraps it
# in Mercury.app (Info.plist + .icns from the icon).  Run on macOS.
# Requires the tool:  go install fyne.io/tools/cmd/fyne@latest
FYNE           ?= fyne
MACOS_APP_NAME ?= Mercury
MACOS_APP_ID   ?= org.rhizomatica.mercury

fyne-ui-macos: libmercury_core.a
	@command -v $(FYNE) >/dev/null 2>&1 || { \
		echo "error: '$(FYNE)' not found — install it with:"; \
		echo "  go install fyne.io/tools/cmd/fyne@latest"; \
		exit 1; }
	@echo "Packaging $(MACOS_APP_NAME).app (macOS)..."
	cd $(FYNE_UI_DIR) && $(FYNE) package -os darwin -tags mercury_embedded \
		-name $(MACOS_APP_NAME) -appID $(MACOS_APP_ID) -icon mercury-ui.png
	@echo "  -> $(FYNE_UI_DIR)/$(MACOS_APP_NAME).app"

# Wrap the .app in a compressed, drag-to-install .dmg (Applications symlink).
# Run on macOS after fyne-ui-macos.  Unsigned — Gatekeeper will warn on first
# open (right-click → Open), which is expected for an unnotarised build.
MACOS_DMG ?= $(MACOS_APP_NAME).dmg
fyne-ui-macos-dmg: fyne-ui-macos
	@echo "Building $(MACOS_DMG)..."
	rm -f $(abspath $(MACOS_DMG))
	rm -rf $(FYNE_UI_DIR)/dmg-stage
	mkdir -p $(FYNE_UI_DIR)/dmg-stage
	cp -R $(FYNE_UI_DIR)/$(MACOS_APP_NAME).app $(FYNE_UI_DIR)/dmg-stage/
	ln -s /Applications $(FYNE_UI_DIR)/dmg-stage/Applications
	hdiutil create -volname "$(MACOS_APP_NAME)" -srcfolder $(FYNE_UI_DIR)/dmg-stage \
		-ov -format UDZO "$(abspath $(MACOS_DMG))"
	rm -rf $(FYNE_UI_DIR)/dmg-stage
	@echo "  -> $(abspath $(MACOS_DMG))"

# ---- Universal (x86_64 + arm64) macOS builds ----
# Each arch is built separately (clang -arch A -> thin objects/binary) and the
# two binaries are lipo-combined; a single -arch x86_64 -arch arm64 link fails
# because intermediate ar archives would hold fat members. The vendored fat
# static hamlib/libusb let ld pick the matching slice for each per-arch link.

# Universal, self-contained mercury CLI (pure C).
macos-universal:
	@for A in x86_64 arm64; do \
		echo "== building mercury slice: $$A =="; \
		$(MAKE) clean >/dev/null; \
		$(MAKE) $(BINARY) CC="clang -arch $$A" || exit 1; \
		mv $(BINARY) mercury-$$A || exit 1; \
	done
	lipo -create mercury-x86_64 mercury-arm64 -output $(BINARY)
	rm -f mercury-x86_64 mercury-arm64
	@echo "  -> $(BINARY)  (universal)"
	@lipo -archs $(BINARY)

# Universal, self-contained Mercury.app: build each arch's mercury-ui against
# the matching per-arch core, lipo the two, then package the prebuilt binary.
fyne-ui-macos-universal:
	@command -v $(FYNE) >/dev/null 2>&1 || { \
		echo "error: '$(FYNE)' not found — install it with:"; \
		echo "  go install fyne.io/tools/cmd/fyne@latest"; \
		exit 1; }
	@for A in x86_64 arm64; do \
		case $$A in x86_64) GOA=amd64;; arm64) GOA=arm64;; esac; \
		echo "== building mercury-ui slice: $$A =="; \
		$(MAKE) clean >/dev/null; \
		$(MAKE) libmercury_core.a CC="clang -arch $$A" || exit 1; \
		( cd $(FYNE_UI_DIR) && CGO_ENABLED=1 GOOS=darwin GOARCH=$$GOA CC="clang -arch $$A" \
			go build -tags mercury_embedded \
			-ldflags "-X main.coreBuildID=$$(cksum $(abspath libmercury_core.a) | cut -d' ' -f1)" \
			-o $(abspath mercury-ui-$$A) . ) || exit 1; \
	done
	lipo -create $(abspath mercury-ui-x86_64) $(abspath mercury-ui-arm64) -output $(abspath mercury-ui)
	rm -f $(abspath mercury-ui-x86_64) $(abspath mercury-ui-arm64)
	@echo "Packaging universal $(MACOS_APP_NAME).app..."
	cd $(FYNE_UI_DIR) && $(FYNE) package -os darwin -tags mercury_embedded \
		-name $(MACOS_APP_NAME) -appID $(MACOS_APP_ID) -icon mercury-ui.png \
		-executable $(abspath mercury-ui)
	@echo "  -> $(FYNE_UI_DIR)/$(MACOS_APP_NAME).app  (universal)"
	@lipo -archs $(FYNE_UI_DIR)/$(MACOS_APP_NAME).app/Contents/MacOS/* || true

# Universal .app wrapped in a drag-to-install .dmg.  The finished .dmg lands at
# the repo top level (e.g. ./Mercury.dmg) — the distribution artifact to upload.
fyne-ui-macos-universal-dmg: fyne-ui-macos-universal
	@echo "Building universal $(MACOS_DMG)..."
	rm -f $(abspath $(MACOS_DMG))
	rm -rf $(FYNE_UI_DIR)/dmg-stage
	mkdir -p $(FYNE_UI_DIR)/dmg-stage
	cp -R $(FYNE_UI_DIR)/$(MACOS_APP_NAME).app $(FYNE_UI_DIR)/dmg-stage/
	ln -s /Applications $(FYNE_UI_DIR)/dmg-stage/Applications
	hdiutil create -volname "$(MACOS_APP_NAME)" -srcfolder $(FYNE_UI_DIR)/dmg-stage \
		-ov -format UDZO "$(abspath $(MACOS_DMG))"
	rm -rf $(FYNE_UI_DIR)/dmg-stage
	@echo "  -> $(abspath $(MACOS_DMG))  (universal)"

# ---- Authenticode signing (Windows binaries) ----
# Two modes:
#   A) WIN_SIGN_PFX  set → sign with a local .pfx file via osslsigncode (self-signed test or OV/EV cert).
#   B) WIN_SIGN_PFX  unset, sign script available → sign via Certum SimplySign cloud (PKCS#11).
#   C) Neither → unsigned build (default, no behaviour change).
#
# For mode B, the in-repo script at SIGN_SCRIPT (code-signing/sign.sh) automates
# the SimplySign Desktop login (Xvfb + xdotool + TOTP) and signs over PKCS#11
# with jsign (SunPKCS11 — the SimplySign module is not enumerable by OpenSC).
# It is OPT-IN: it only runs when CERTUM_EMAIL is set in the environment
# (together with CERTUM_OTP_URI_FILE), so plain `windows-zip` stays unsigned by
# default even though the script is present.  See docs/WINDOWS-SIGNING.md.
#
WIN_SIGN_PFX  ?=
WIN_SIGN_PASS ?=
WIN_SIGN_TS   ?= http://timestamp.digicert.com
WIN_SIGN_NAME ?= Mercury HF Modem
WIN_SIGN_URL  ?= https://github.com/Rhizomatica/mercury
SIGN_SCRIPT   ?= $(CURDIR)/code-signing/sign.sh
SIGN_LOGOUT   ?= $(CURDIR)/code-signing/sign-logout.sh

# $(call win_sign,file) — sign in place if a signing method is available.
define win_sign
	@if [ -n "$(WIN_SIGN_PFX)" ]; then \
		echo "Signing (pfx): $(1)"; \
		osslsigncode sign -pkcs12 "$(WIN_SIGN_PFX)" -pass "$(WIN_SIGN_PASS)" \
			-n "$(WIN_SIGN_NAME)" -i "$(WIN_SIGN_URL)" -h sha256 -ts "$(WIN_SIGN_TS)" \
			-in "$(1)" -out "$(1).signed" && mv -f "$(1).signed" "$(1)"; \
	elif [ -x "$(SIGN_SCRIPT)" ] && [ -n "$$CERTUM_EMAIL" ]; then \
		echo "Signing (SimplySign cloud): $(1)"; \
		$(SIGN_SCRIPT) "$(1)"; \
	else \
		echo "WARNING: no signing method available — $(1) is unsigned"; \
	fi
endef

# ---- Windows Authenticode signing targets ----
# Sign the already-built mercury.exe (SimplySign cloud via PKCS#11).
# Requires: sign.sh script, xvfb, fluxbox, xdotool, opensc, osslsigncode.
sign-windows: mercury.exe
	$(call win_sign,mercury.exe)

# Sign an arbitrary .exe (e.g. make sign-windows-bin BIN=mercury-ui.exe)
sign-windows-bin:
	$(call win_sign,$(BIN))

# Build + sign both payload .exe files + zip.
windows-zip-signed: windows fyne-ui-windows
	rm -rf $(WINDOWS_DIR) $(WINDOWS_ZIP)
	mkdir -p $(WINDOWS_DIR)
	cp mercury.exe $(WINDOWS_DIR)/
	cp $(WINDOWS_INSTALLER_DIR)/$(FYNE_UI_BIN) $(WINDOWS_DIR)/
	$(call win_sign,$(WINDOWS_DIR)/mercury.exe)
	$(call win_sign,$(WINDOWS_DIR)/$(FYNE_UI_BIN))
	@[ -x "$(SIGN_LOGOUT)" ] && [ -n "$$CERTUM_EMAIL" ] && $(SIGN_LOGOUT) || true
	cp mercury.ini.example $(WINDOWS_DIR)/
	if ls $(HAMLIB_W64_DIR)/bin/*.dll >/dev/null 2>&1; then \
		cp $(HAMLIB_W64_DIR)/bin/*.dll $(WINDOWS_DIR)/; \
	fi
	zip -9r $(WINDOWS_ZIP) $(WINDOWS_DIR)
	rm -rf $(WINDOWS_DIR)
	@echo "Created $(WINDOWS_ZIP) (signed)"

# Build + sign the installer, PAYLOAD FIRST.
#
# The order is the whole point.  windows-installer stages mercury-ui.exe into
# $(WINDOWS_INSTALLER_DIR) and ISCC packs whatever is sitting there, so signing
# only the finished Setup.exe ships a signed installer that installs an
# UNSIGNED program: the publisher shows on the download, then disappears at the
# moment the user actually runs the thing.  Sign the payload, then build, then
# sign the installer — and do it in one SimplySign session.
#
# ISCC is a Windows tool.  Point ISCC at it if you have Inno Setup under Wine
# (ISCC='wine ~/.wine/drive_c/Program Files (x86)/Inno Setup 6/ISCC.exe'), or
# leave it unset to have this target stop after signing the payload and print
# the two commands to finish on a Windows box.
ISCC ?=
# installer.iss sets no OutputDir, so Inno writes next to the script in Output/.
# Located by glob after the build rather than hardcoded, so a future OutputDir
# or version change cannot leave us signing a stale file — or nothing at all.
WINDOWS_SETUP_GLOB = $(WINDOWS_INSTALLER_DIR)/Output/Mercury_*_Setup.exe

windows-installer-signed: windows-installer
	$(call win_sign,$(WINDOWS_INSTALLER_DIR)/$(FYNE_UI_BIN))
	@rm -f $(WINDOWS_SETUP_GLOB)
	@if [ -z "$(ISCC)" ]; then \
		[ -x "$(SIGN_LOGOUT)" ] && [ -n "$$CERTUM_EMAIL" ] && $(SIGN_LOGOUT) || true; \
		echo ""; \
		echo "Payload $(FYNE_UI_BIN) staged in $(WINDOWS_INSTALLER_DIR)/ (see the signing result above)."; \
		echo "ISCC is not set, so the installer itself was not built.  Finish with:"; \
		echo "  ISCC $(WINDOWS_INSTALLER_DIR)/installer.iss"; \
		echo "  make sign-windows-bin BIN=$(WINDOWS_INSTALLER_DIR)/Output/Mercury_$(MERCURY_VERSION)_Setup.exe"; \
		echo ""; \
	else \
		echo "Building installer with ISCC..."; \
		$(ISCC) $(WINDOWS_INSTALLER_DIR)/installer.iss || exit 1; \
		setup=$$(ls $(WINDOWS_SETUP_GLOB) 2>/dev/null | head -1); \
		if [ -z "$$setup" ]; then \
			echo "ERROR: ISCC produced no $(WINDOWS_SETUP_GLOB)"; exit 1; \
		fi; \
		$(MAKE) --no-print-directory sign-windows-bin BIN="$$setup" || exit 1; \
		[ -x "$(SIGN_LOGOUT)" ] && [ -n "$$CERTUM_EMAIL" ] && $(SIGN_LOGOUT) || true; \
		echo "Created $$setup (installer and payload both signed)"; \
	fi

windows-zip: windows fyne-ui-windows
	rm -rf $(WINDOWS_DIR) $(WINDOWS_ZIP)
	mkdir -p $(WINDOWS_DIR)
	cp mercury.exe $(WINDOWS_DIR)/
	cp $(WINDOWS_INSTALLER_DIR)/$(FYNE_UI_BIN) $(WINDOWS_DIR)/
	$(call win_sign,$(WINDOWS_DIR)/mercury.exe)
	$(call win_sign,$(WINDOWS_DIR)/$(FYNE_UI_BIN))
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
		go build -tags mercury_embedded -ldflags="-s -w -H windowsgui -X main.coreBuildID=$$(cksum $(abspath libmercury_core_w64.a) | cut -d' ' -f1)" -o $(abspath $(WINDOWS_INSTALLER_DIR)/$(FYNE_UI_BIN)) .
	@echo "  -> $(WINDOWS_INSTALLER_DIR)/$(FYNE_UI_BIN)"

windows-installer: fyne-ui-windows
	cp mercury.ini.example $(WINDOWS_INSTALLER_DIR)/mercury.ini
	sed -i 's/ui_enabled = false/ui_enabled = true/g' $(WINDOWS_INSTALLER_DIR)/mercury.ini
	sed -i 's/sound_system = auto/sound_system = wasapi/g' $(WINDOWS_INSTALLER_DIR)/mercury.ini
	if ls $(HAMLIB_W64_DIR)/bin/*.dll >/dev/null 2>&1; then \
		cp $(HAMLIB_W64_DIR)/bin/*.dll $(WINDOWS_INSTALLER_DIR)/; \
	fi
	@echo "windows-installer ready."
	@echo ""
	@echo "Build the installer (Windows or Wine):"
	@echo "  ISCC $(WINDOWS_INSTALLER_DIR)/installer.iss"
	@echo ""
	@echo "Then sign the resulting .exe on Linux:"
	@echo "  make sign-windows-bin BIN=Mercury_\$$(VERSION)_Setup.exe"
	@echo ""

clean:
	rm -f mercury mercury.exe *.o .git_hash_stamp mercury-*.zip libmercury_core.a libmercury_core_w64.a
	rm -rf mercury-[0-9]*
	rm -f $(WINDOWS_INSTALLER_DIR)/$(FYNE_UI_BIN)
	rm -f $(FYNE_UI_DIR)/engine/mercury_bridge.o $(FYNE_UI_DIR)/engine/mercury_bridge_w64.o
	rm -f mercury-ui $(FYNE_UI_DIR)/mercury-ui $(FYNE_UI_DIR)/mercury-fyne-ui
	rm -f $(MACOS_DMG) $(FYNE_UI_DIR)/$(MACOS_DMG)
	rm -rf $(FYNE_UI_DIR)/$(MACOS_APP_NAME).app $(FYNE_UI_DIR)/dmg-stage
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
