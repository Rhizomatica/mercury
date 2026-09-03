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

# hidapi: OPTIONAL, and preferred when present.  It is what gives the CM108
# GPIO PTT backend Windows and macOS support; without it the backend falls back
# to talking to /dev/hidraw directly, which works on Linux only.  Deliberately
# not a hard dependency -- a minimal Raspberry Pi build should not need another
# package to key a radio.  hidraw first: on Linux it needs no libusb detach and
# co-exists with the kernel's own driver.
HAVE_HIDAPI :=
HIDAPI_CFLAGS =
HIDAPI_LDFLAGS =
HIDAPI_OBJS =
HIDAPI_W64_DIR = radio_io/hidapi-w64
HIDAPI_MACOS_DIR = radio_io/hidapi-macos
ifeq ($(OS),Windows_NT)
  # Vendored, like hamlib-w64: pkg-config on a cross-build would find the HOST
  # hidapi and link something that cannot run on the target.  hidapi's Windows
  # backend is self-contained source over setupapi/cfgmgr32, so it builds with
  # the rest of the tree rather than needing a prebuilt library.
  ifneq ($(wildcard $(HIDAPI_W64_DIR)/src/hid.c),)
    HAVE_HIDAPI := 1
    HIDAPI_CFLAGS := -I$(HIDAPI_W64_DIR)/include -DHAVE_HIDAPI
    HIDAPI_LDFLAGS := -lsetupapi -lcfgmgr32 -lhid
    HIDAPI_OBJS := $(HIDAPI_W64_DIR)/hid.o
  endif
else
  # macOS: vendored first, like hamlib-macos.  A stock macOS has neither
  # Homebrew nor pkg-config, so a pkg-config-only probe silently compiles the
  # CM108 backend out, and the binary then tells the user CM108 is "Linux only"
  # -- on a platform we claim to support.  hidapi's macOS backend is
  # self-contained source over IOKit, so it builds with the rest of the tree.
  ifeq ($(UNAME_S),Darwin)
    ifneq ($(wildcard $(HIDAPI_MACOS_DIR)/src/hid.c),)
      HAVE_HIDAPI := 1
      HIDAPI_CFLAGS := -I$(HIDAPI_MACOS_DIR)/include -DHAVE_HIDAPI
      HIDAPI_LDFLAGS := -framework IOKit -framework CoreFoundation -framework AppKit
      HIDAPI_OBJS := $(HIDAPI_MACOS_DIR)/hid.o
    endif
  endif
  # Linux, and macOS without the vendored copy: system package via pkg-config.
  ifneq ($(HAVE_HIDAPI),1)
    HIDAPI_PC := $(firstword $(foreach m,hidapi-hidraw hidapi-libusb hidapi,\
                   $(shell pkg-config --exists $(m) 2>/dev/null && echo $(m))))
    ifneq ($(strip $(HIDAPI_PC)),)
      HAVE_HIDAPI := 1
      HIDAPI_CFLAGS := $(shell pkg-config --cflags $(HIDAPI_PC)) -DHAVE_HIDAPI
      HIDAPI_LDFLAGS := $(shell pkg-config --libs $(HIDAPI_PC))
    endif
  endif
endif


export HAVE_HAMLIB
export HAVE_HERMES_SHM
export HAVE_HIDAPI
export HIDAPI_CFLAGS
export HIDAPI_LDFLAGS
export HIDAPI_OBJS

include config.mk

MINGW_CC  = x86_64-w64-mingw32-gcc
MINGW_AR  = x86_64-w64-mingw32-ar

FYNE_UI_DIR   = gui_interface/fyne-ui
FYNE_UI_BIN   = mercury-ui.exe
MINGW_GO_CC   = x86_64-w64-mingw32-gcc

.PHONY: all install internal_deps utils clean doxygen doxygen-clean windows windows-zip windows-installer-signed windows-installer-stage check-installer-names fyne-ui fyne-ui-macos fyne-ui-macos-dmg macos-universal fyne-ui-macos-universal fyne-ui-macos-universal-dmg sign-macos-bin macos-notarize-dmg fyne-ui-windows windows-installer test integration-test FORCE

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

LDFLAGS=$(FFAUDIO_LINKFLAGS) -lm $(HAMLIB_LDFLAGS) $(HIDAPI_LDFLAGS) $(ATOMIC_LDFLAGS)

MERCURY_LINK_INPUTS = \
	main.o common/cfg_utils.o common/iniparser/iniparser.o common/iniparser/dictionary.o \
	datalink_arq/arq.o datalink_arq/arq_tnc.o datalink_arq/arith.o datalink_arq/arq_channels.o \
	datalink_arq/arq_fsm.o datalink_arq/arq_protocol.o datalink_arq/arq_timing.o datalink_arq/arq_modem.o \
	datalink_broadcast/broadcast.o datalink_broadcast/kiss.o modem/modem.o modem/framer.o modem/channel_busy.o modem/freedv/libfreedvdata.a \
	audioio/audioio.a common/os_interop.o common/ring_buffer_posix.o common/shm_posix.o common/crc6.o common/hermes_log.o common/virtual_clock.o \
	common/chan.o common/queue.o common/mercury_engine.o common/mercury_cli.o data_interfaces/tcp_interfaces.o data_interfaces/net.o \
	gui_interface/ui_communication.o gui_interface/ui_status.o gui_interface/ui_devices.o \
	gui_interface/websocket/mongoose.o gui_interface/websocket/mercury_websocket.o \
	gui_interface/websocket/web_packed.o \
	radio_io/radio_io.o radio_io/serial_ptt.o radio_io/cm108_ptt.o $(HIDAPI_OBJS)

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

# Vendored hidapi for the Windows cross-build (see the detection block above).
# Built with $(MINGW_CC) rather than $(CC) so it is correct both for the native
# `windows` target (where CC is already $(MINGW_CC)) and for the
# `fyne-ui-windows` cross-build (whose host context would otherwise compile a
# native ELF object that the mingw linker rejects).
$(HIDAPI_W64_DIR)/hid.o: $(HIDAPI_W64_DIR)/src/hid.c
	$(MINGW_CC) -O2 -I$(HIDAPI_W64_DIR)/include -I$(HIDAPI_W64_DIR)/src -c $< -o $@

# Vendored hidapi for macOS: the IOKit backend, built from source so a stock
# macOS with only the Xcode command line tools still gets CM108 PTT.
$(HIDAPI_MACOS_DIR)/hid.o: $(HIDAPI_MACOS_DIR)/src/hid.c
	$(CC) -O2 -I$(HIDAPI_MACOS_DIR)/include -c $< -o $@

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

# RaptorQ (vendored nanorq) and the broadcast file carousel.  Only
# libmercury_core needs these -- the standalone daemon does not transmit files.
RAPTORQ_SRCS = $(wildcard datalink_broadcast/raptorq/lib/*.c) \
               $(wildcard datalink_broadcast/raptorq/deps/obl/*.c)
RAPTORQ_CFLAGS = -Idatalink_broadcast/raptorq/include -Idatalink_broadcast/raptorq/deps

# Native and Windows objects go to DIFFERENT paths.
#
# The cross build compiles the same sources with the mingw compiler, and if both
# wrote to %.o the second build would leave the first's archive full of objects
# for the wrong platform -- silently, because make sees an .o newer than its .c
# and considers it up to date.  That shows up much later as a pile of
# "undefined reference to __imp__assert" at link time.  Distinct suffixes make
# the two builds simply independent.
RAPTORQ_OBJS     = $(patsubst %.c,%.o,$(RAPTORQ_SRCS))
RAPTORQ_OBJS_W64 = $(patsubst %.c,%.w64.o,$(RAPTORQ_SRCS))
BCAST_FILE_OBJS     = datalink_broadcast/bcast_file.o $(RAPTORQ_OBJS)
BCAST_FILE_OBJS_W64 = datalink_broadcast/bcast_file.w64.o $(RAPTORQ_OBJS_W64)

$(RAPTORQ_OBJS): %.o: %.c
	$(CC) $(CFLAGS) $(RAPTORQ_CFLAGS) -c $< -o $@

$(RAPTORQ_OBJS_W64): %.w64.o: %.c
	$(MINGW_CC) $(CFLAGS) $(RAPTORQ_CFLAGS) -c $< -o $@

datalink_broadcast/bcast_file.o: datalink_broadcast/bcast_file.c
	$(CC) $(CFLAGS) $(RAPTORQ_CFLAGS) -c $< -o $@

datalink_broadcast/bcast_file.w64.o: datalink_broadcast/bcast_file.c
	$(MINGW_CC) $(CFLAGS) $(RAPTORQ_CFLAGS) -c $< -o $@

MERCURY_CORE_OBJS = \
	common/cfg_utils.o common/iniparser/iniparser.o common/iniparser/dictionary.o \
	datalink_arq/arq.o datalink_arq/arq_tnc.o datalink_arq/arith.o datalink_arq/arq_channels.o \
	datalink_arq/arq_fsm.o datalink_arq/arq_protocol.o datalink_arq/arq_timing.o datalink_arq/arq_modem.o \
	datalink_broadcast/broadcast.o datalink_broadcast/kiss.o \
	modem/modem.o modem/framer.o modem/channel_busy.o \
	common/os_interop.o common/ring_buffer_posix.o common/shm_posix.o common/crc6.o common/hermes_log.o common/virtual_clock.o \
	common/chan.o common/queue.o common/mercury_engine.o common/mercury_cli.o \
	data_interfaces/tcp_interfaces.o data_interfaces/net.o \
	gui_interface/ui_communication.o gui_interface/ui_status.o gui_interface/ui_devices.o \
	gui_interface/websocket/mongoose.o gui_interface/websocket/mercury_websocket.o \
	gui_interface/websocket/web_packed.o \
	radio_io/radio_io.o radio_io/serial_ptt.o radio_io/cm108_ptt.o $(HIDAPI_OBJS)

ifeq ($(HAVE_HERMES_SHM),1)
MERCURY_CORE_OBJS += radio_io/sbitx_io.o radio_io/shm_utils.o
endif

ifeq ($(HAVE_HAMLIB),1)
MERCURY_CORE_OBJS += radio_io/rigctl_parse.o
endif

# Simply-expanded (:=), so the dedupe below can refer to it without make
# complaining that the variable references itself.
MERCURY_CORE_OBJS_W64 := $(filter-out radio_io/sbitx_io.o radio_io/shm_utils.o,$(MERCURY_CORE_OBJS))

# rigctl_parse.o may already be here, inherited from MERCURY_CORE_OBJS when the
# HOST has hamlib.  Adding it unconditionally put it in the archive twice --
# two identical members, which GNU ld tolerates but a stricter linker need not,
# and which is simply wrong either way.  Add it only when it is not already
# present, so the Windows archive is correct whether or not the host has hamlib.
ifneq ($(strip $(HAMLIB_W64_LIBS)),)
MERCURY_CORE_OBJS_W64 += $(filter-out $(MERCURY_CORE_OBJS_W64),radio_io/rigctl_parse.o)
endif

# $(HIDAPI_OBJS) is listed in MERCURY_CORE_OBJS but is NOT built by
# internal_deps -- the vendored hidapi object has its own top-level rule, and
# radio_io's sub-make does not build it either.  Without it as a prerequisite
# the archive step just assumed the file was lying around from an earlier
# build, which it was until `clean` started removing it properly:
#     ar: radio_io/hidapi-macos/hid.o: No such file or directory
# $(BINARY) already declares it via MERCURY_LINK_INPUTS, which is why the CLI
# build was unaffected and only the .app/.dmg packaging path broke.
libmercury_core.a: internal_deps $(HIDAPI_OBJS) $(BCAST_FILE_OBJS)
	$(CC) $(CFLAGS) $(RAPTORQ_CFLAGS) -I. -c $(FYNE_UI_DIR)/engine/mercury_bridge.c -o $(FYNE_UI_DIR)/engine/mercury_bridge.o
	# Remove a stale archive first: macOS ar (cctools) refuses to update an
	# existing *fat* .a in place, so a leftover universal build would wedge the
	# next native build ("is a fat file"). Fresh create is identical on Linux.
	rm -f $@
	$(AR) rcs $@ $(MERCURY_CORE_OBJS) $(BCAST_FILE_OBJS) $(FYNE_UI_DIR)/engine/mercury_bridge.o

# The OS=Windows_NT internal_deps sub-make compiles cm108_ptt.o with
# -DHAVE_HIDAPI, so the archive must carry the matching vendored hidapi object.
# It can't come from $(HIDAPI_OBJS): that is evaluated in this (host) context,
# where it is empty on Linux.  Resolve the vendored object directly, mirroring
# the wildcard guard in the hidapi detection block above.
HIDAPI_W64_OBJ :=
ifneq ($(wildcard $(HIDAPI_W64_DIR)/src/hid.c),)
HIDAPI_W64_OBJ := $(HIDAPI_W64_DIR)/hid.o
endif

libmercury_core_w64.a: $(HIDAPI_W64_OBJ)
	$(MAKE) internal_deps OS=Windows_NT CC=$(MINGW_CC) AR=$(MINGW_AR) HAVE_HERMES_SHM=0
	# The bridge calls bcast_file_*, so the RaptorQ objects belong in this
	# archive too -- built with the cross compiler into their own .w64.o paths,
	# so a native build afterwards is not left linking Windows objects.
	$(MAKE) $(BCAST_FILE_OBJS_W64) OS=Windows_NT HAVE_HERMES_SHM=0
	$(MINGW_CC) $(CFLAGS) $(RAPTORQ_CFLAGS) -I. -c $(FYNE_UI_DIR)/engine/mercury_bridge.c -o $(FYNE_UI_DIR)/engine/mercury_bridge_w64.o
	$(MINGW_AR) rcs $@ $(MERCURY_CORE_OBJS_W64) $(HIDAPI_W64_OBJ) $(BCAST_FILE_OBJS_W64) $(FYNE_UI_DIR)/engine/mercury_bridge_w64.o

# HIDAPI_LDFLAGS is passed through CGO_LDFLAGS rather than hardcoded in
# mercury_link_linux.go's #cgo directive, because hidapi is OPTIONAL: only this
# Makefile knows whether pkg-config found it.  Without this, cm108_ptt.o's
# hid_* references go unresolved on any host that HAS hidapi installed.
fyne-ui: libmercury_core.a
	@echo "Building Mercury UI (native: Linux or macOS)..."
	cd $(FYNE_UI_DIR) && CGO_ENABLED=1 CGO_LDFLAGS="$(HIDAPI_LDFLAGS)" go build -tags mercury_embedded \
		-ldflags "-X main.coreBuildID=$$(cksum $(abspath libmercury_core.a) | cut -d' ' -f1)" \
		-o $(abspath mercury-ui) .
	@echo "  -> mercury-ui"

# macOS .app bundle.  Fyne's packaging tool builds the binary (with the
# mercury_embedded tag, so it links via mercury_link_darwin.go) and wraps it
# in Mercury.app (Info.plist + .icns from the icon).  Run on macOS.
# Requires the tool:  go install fyne.io/tools/cmd/fyne@latest
PLIST_BUDDY    ?= /usr/libexec/PlistBuddy
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
	$(PLIST_BUDDY) -c "Add :NSMicrophoneUsageDescription string 'Mercury needs access to the radio audio to listen for data signals'" $(FYNE_UI_DIR)/$(MACOS_APP_NAME).app/Contents/Info.plist
	@echo "  -> $(FYNE_UI_DIR)/$(MACOS_APP_NAME).app"

# Wrap the .app in a compressed, drag-to-install .dmg (Applications symlink).
# Run on macOS after fyne-ui-macos.  Unsigned — Gatekeeper will warn on first
# open (right-click → Open), which is expected for an unnotarised build.
# Version-stamped, like the Windows artifacts (mercury-$(MERCURY_VERSION)-w64-*.zip
# and Mercury_$(MERCURY_VERSION)_Setup.exe).  A bare Mercury.dmg is impossible to
# tell apart from the previous one in a downloads folder or on a release page.
MACOS_DMG           ?= $(MACOS_APP_NAME)-$(MERCURY_VERSION).dmg
MACOS_DMG_UNIVERSAL ?= $(MACOS_APP_NAME)-$(MERCURY_VERSION)-universal.dmg
MACOS_CLI_PARK       = mercury-cli-universal

# ---- macOS code signing (rcodesign) --------------------------------------
# github.com/indygreg/apple-platform-rs -- a pure-Rust reimplementation that
# signs Mach-O binaries, BUNDLES, .dmg images and .pkg archives, and notarizes
# and staples, with no Mac, no Xcode and no keychain.  Used instead of Apple's
# codesign so the signing certificate never has to reach a macOS runner, and
# instead of anchore/quill, which cannot sign bundles or disk images at all
# (anchore/quill#815, #550, both open) -- the two things Gatekeeper judges.
#
# Verified: a Mercury.app signed by rcodesign 0.28.0 on Linux passes Apple's
# own `codesign --verify --deep --strict` on macOS 15.7.7 ("valid on disk",
# "satisfies its Designated Requirement"), with CodeResources written and the
# universal Mach-O sealed.
#
# NOTE this does not remove macOS from the release entirely: hdiutil builds the
# .dmg and is Apple-only.  What moves off the Mac is signing and notarization.
#
#   MACOS_SIGN_P12 / MACOS_SIGN_P12_PASSWORD    Developer ID cert
#
# Opt-in, same shape as win_sign: with no certificate the build still completes
# and says so, keeping developer builds unchanged.  0.28.0 is the version
# pinned by indygreg/apple-code-sign-action.
RCODESIGN ?= rcodesign

# $(call macos_sign,<mach-o | bundle dir | dmg>)
# --code-signature-flags runtime is the hardened runtime, which notarization
# requires; Apple rejects the upload without it.
define macos_sign
	@if [ -n "$(MACOS_SIGN_P12)" ]; then \
		command -v $(RCODESIGN) >/dev/null 2>&1 || { \
			echo "error: MACOS_SIGN_P12 is set but '$(RCODESIGN)' is not installed"; \
			echo "  https://github.com/indygreg/apple-platform-rs/releases"; \
			exit 1; }; \
		echo "Signing (rcodesign): $(1)"; \
		$(RCODESIGN) sign \
			--p12-file "$(MACOS_SIGN_P12)" \
			--p12-password "$(MACOS_SIGN_P12_PASSWORD)" \
			--code-signature-flags runtime \
			$(if $(2),--binary-identifier "$(2)",) \
			"$(1)" || exit 1; \
	else \
		echo "WARNING: MACOS_SIGN_P12 unset — $(1) is unsigned"; \
	fi
endef

# Sign an already-built artifact by hand (binary, .app or .dmg):
#   make sign-macos-bin BIN=mercury MACOS_SIGN_P12=cert.p12 MACOS_SIGN_P12_PASSWORD=...
sign-macos-bin:
	$(call macos_sign,$(BIN))
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
		$(MAKE) internal_deps CC="clang -arch $$A" || exit 1; \
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
	$(PLIST_BUDDY) -c "Add :NSMicrophoneUsageDescription string 'Mercury needs access to the radio audio to listen for data signals'" $(FYNE_UI_DIR)/$(MACOS_APP_NAME).app/Contents/Info.plist
	@echo "  -> $(FYNE_UI_DIR)/$(MACOS_APP_NAME).app  (universal)"
	@lipo -archs $(FYNE_UI_DIR)/$(MACOS_APP_NAME).app/Contents/MacOS/* || true

# Universal .app wrapped in a drag-to-install .dmg.  The finished .dmg lands at
# the repo top level (e.g. ./Mercury-1.9.11-universal.dmg) — the artifact to
# upload.
#
# The image also carries the headless CLI, which it did not before: the Windows
# zip has always shipped mercury.exe next to the GUI, but the Mac image held
# only Mercury.app, so a Mac operator wanting a TNC/uucp station had nothing to
# install.  The GUI is built -tags mercury_embedded (the modem is linked into
# it), so the CLI is a genuinely separate artifact, not a duplicate of it.
#
# It is staged NEXT TO the .app rather than inside Contents/MacOS: a drag-install
# still copies exactly one thing, and stray executables inside a bundle are the
# kind of thing that complicates signing/notarisation later.
#
# Ordering here is deliberate and cannot be expressed as prerequisites: BOTH
# universal targets run `make clean` between their two arch slices, and clean
# removes `mercury` AND Mercury.app — so whichever ran second would delete what
# the first produced.  Build the CLI first, park it under a name clean does not
# match, then package the .app.
fyne-ui-macos-universal-dmg:
	rm -f $(MACOS_CLI_PARK)
	$(MAKE) macos-universal
	mv $(BINARY) $(MACOS_CLI_PARK)
	$(MAKE) fyne-ui-macos-universal
	@echo "Building universal $(MACOS_DMG_UNIVERSAL)..."
	rm -f $(abspath $(MACOS_DMG_UNIVERSAL))
	rm -rf $(FYNE_UI_DIR)/dmg-stage
	mkdir -p $(FYNE_UI_DIR)/dmg-stage
	cp -R $(FYNE_UI_DIR)/$(MACOS_APP_NAME).app $(FYNE_UI_DIR)/dmg-stage/
	ln -s /Applications $(FYNE_UI_DIR)/dmg-stage/Applications
	mkdir -p "$(FYNE_UI_DIR)/dmg-stage/Command Line"
	cp $(abspath $(MACOS_CLI_PARK)) "$(FYNE_UI_DIR)/dmg-stage/Command Line/$(BINARY)"
	$(call macos_sign,$(FYNE_UI_DIR)/dmg-stage/Command Line/$(BINARY))
	cp mercury.ini.example "$(FYNE_UI_DIR)/dmg-stage/Command Line/"
	printf '%s\n' \
	  'Mercury $(MERCURY_VERSION) - command-line (headless) modem' \
	  '' \
	  'Mercury.app next to this folder is the GUI and needs nothing else.' \
	  'This folder is for running Mercury headless: as a TNC for Winlink/BPQ32,' \
	  'or under uucp.' \
	  '' \
	  'Install:' \
	  '  sudo cp mercury /usr/local/bin/' \
	  '  cp mercury.ini.example ~/.mercury.ini    # then edit for your radio' \
	  '  mercury -h                               # options' \
	  '' \
	  'Universal binary (Intel + Apple Silicon).  If this build is unsigned, the' \
	  'first run needs:  xattr -d com.apple.quarantine /usr/local/bin/mercury' \
	  > "$(FYNE_UI_DIR)/dmg-stage/Command Line/README.txt"
	@# Seal the bundle before it goes into the image: rcodesign recurses into
	@# nested Mach-Os and writes Contents/_CodeSignature/CodeResources.
	@# Signing the image afterwards does NOT sign what is inside it.
	$(call macos_sign,$(FYNE_UI_DIR)/dmg-stage/$(MACOS_APP_NAME).app)
	hdiutil create -volname "$(MACOS_APP_NAME) $(MERCURY_VERSION)" \
		-srcfolder $(FYNE_UI_DIR)/dmg-stage \
		-ov -format UDZO "$(abspath $(MACOS_DMG_UNIVERSAL))"
	$(call macos_sign,$(abspath $(MACOS_DMG_UNIVERSAL)),$(MACOS_APP_ID))
	rm -rf $(FYNE_UI_DIR)/dmg-stage
	rm -f $(abspath $(MACOS_CLI_PARK))
	@echo "  -> $(abspath $(MACOS_DMG_UNIVERSAL))  (universal, GUI + CLI)"

# ---- Notarization (rcodesign; network, no Mac) ---------------------------
# Separate from signing on purpose: signing is local and offline, this uploads
# to Apple, waits for the verdict and staples the ticket into the image so
# Gatekeeper accepts it offline afterwards.
#
# Credentials are an App Store Connect API key, encoded once into a JSON file:
#   rcodesign encode-app-store-connect-api-key -o ~/.mercury-notary.json \
#       <issuer-id> <key-id> /path/to/AuthKey_<key-id>.p8
#
# then:  make macos-notarize-dmg MACOS_NOTARY_KEY=~/.mercury-notary.json
MACOS_NOTARY_KEY ?=
macos-notarize-dmg:
	@[ -n "$(MACOS_NOTARY_KEY)" ] || { \
		echo "error: set MACOS_NOTARY_KEY to an encoded App Store Connect key"; \
		echo "  rcodesign encode-app-store-connect-api-key -o key.json <issuer> <key-id> AuthKey.p8"; \
		exit 1; }
	@[ -f "$(MACOS_DMG_UNIVERSAL)" ] || { \
		echo "error: $(MACOS_DMG_UNIVERSAL) not built yet"; exit 1; }
	$(RCODESIGN) notary-submit \
		--api-key-file "$(MACOS_NOTARY_KEY)" --staple \
		"$(MACOS_DMG_UNIVERSAL)"
	@echo "  -> $(abspath $(MACOS_DMG_UNIVERSAL))  (notarized + stapled)"

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
# ISCC is the Inno Setup compiler, a Windows program: on Linux set ISCC_RUN=wine
# alongside it.  Both are quoted at the point of use, so the default install
# path — which contains a space AND parentheses, "Program Files (x86)" — works
# without the caller escaping anything.  (Passing one combined command string
# does not: the unquoted parens are a shell syntax error.)
#
#   make windows-installer-signed ISCC_RUN=wine \
#        ISCC="$HOME/.wine/drive_c/Program Files (x86)/Inno Setup 6/ISCC.exe"
#
# Leave ISCC unset to stop after signing the payload and print the two commands
# to finish on a Windows box.
ISCC     ?=
ISCC_RUN ?=
# installer.iss sets OutputDir=.. so the installer lands in the project root,
# beside the release ZIP.  Located by glob after the build rather than
# hardcoded, so a version change cannot leave us signing a stale file — or
# nothing at all.
WINDOWS_SETUP_GLOB = Mercury_*_Setup.exe

windows-installer-signed: windows-installer
	$(call win_sign,$(WINDOWS_INSTALLER_DIR)/$(FYNE_UI_BIN))
	$(call win_sign,$(WINDOWS_INSTALLER_DIR)/mercury.exe)
	@rm -f $(WINDOWS_SETUP_GLOB)
	@if [ -z "$(ISCC)" ]; then \
		[ -x "$(SIGN_LOGOUT)" ] && [ -n "$$CERTUM_EMAIL" ] && $(SIGN_LOGOUT) || true; \
		echo ""; \
		echo "Payloads $(FYNE_UI_BIN) + mercury.exe staged in $(WINDOWS_INSTALLER_DIR)/ (see the signing results above)."; \
		echo "ISCC is not set, so the installer itself was not built.  Finish with:"; \
		echo "  ISCC $(WINDOWS_INSTALLER_DIR)/installer.iss"; \
		echo "  make sign-windows-bin BIN=Mercury_\$$(VERSION)_Setup.exe"; \
		echo ""; \
	else \
		echo "Building installer with ISCC..."; \
		if [ -n "$(ISCC_RUN)" ]; then \
			"$(ISCC_RUN)" "$(ISCC)" "$(WINDOWS_INSTALLER_DIR)/installer.iss" || exit 1; \
		else \
			"$(ISCC)" "$(WINDOWS_INSTALLER_DIR)/installer.iss" || exit 1; \
		fi; \
		setup=$$(ls $(WINDOWS_SETUP_GLOB) 2>/dev/null | head -1); \
		if [ -z "$$setup" ]; then \
			echo "ERROR: ISCC produced no $(WINDOWS_SETUP_GLOB)"; exit 1; \
		fi; \
		$(MAKE) --no-print-directory sign-windows-bin BIN="$$setup" || exit 1; \
		[ -x "$(SIGN_LOGOUT)" ] && [ -n "$$CERTUM_EMAIL" ] && $(SIGN_LOGOUT) || true; \
		if [ -n "$$CERTUM_EMAIL" ] || [ -n "$(WIN_SIGN_PFX)" ]; then \
			echo "Created $$setup (payload and installer signed)"; \
		else \
			echo "Created $$setup — UNSIGNED: no signing method configured"; \
			echo "  (set CERTUM_EMAIL for SimplySign, or WIN_SIGN_PFX for a local .pfx)"; \
		fi; \
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
		go build -tags mercury_embedded -buildvcs=false -ldflags="-s -w -H windowsgui -X main.coreBuildID=$$(cksum $(abspath libmercury_core_w64.a) | cut -d' ' -f1)" -o $(abspath $(WINDOWS_INSTALLER_DIR)/$(FYNE_UI_BIN)) .
	@echo "  -> $(WINDOWS_INSTALLER_DIR)/$(FYNE_UI_BIN)"

# Stage everything the installer packs, WITHOUT rebuilding the binaries.
# Split out so a signing environment can reuse payloads it has just signed:
# the release pipeline signs in a container that has the certificate but no
# mingw/Go toolchain, so it must be able to pack without triggering a rebuild
# (which would also discard the signatures it just applied).
# Windows rejects \ / : * ? " < > | in file names, and Inno Setup only finds
# out at INSTALL time — ISCC compiles a bad shortcut name happily and the user
# gets "IPersistFile::Save failed; code 0x80070003" halfway through Setup.
# Nothing in CI installs the installer, so check the names statically.
check-installer-names:
	@awk -F'"' '/^Name: "\{(group|autodesktop|commondesktop|userdesktop)\}/ { \
		n = $$2; sub(/.*\\/, "", n); \
		if (n ~ /[\/:*?<>|]/) { \
			printf("%s:%d: illegal character in shortcut name: %s\n", FILENAME, NR, n); \
			rc = 1 } } \
	    END { exit rc }' $(WINDOWS_INSTALLER_DIR)/installer.iss

windows-installer-stage: check-installer-names
	cp mercury.exe $(WINDOWS_INSTALLER_DIR)/
	cp mercury.ini.example $(WINDOWS_INSTALLER_DIR)/mercury.ini
	sed -i 's/ui_enabled = false/ui_enabled = true/g' $(WINDOWS_INSTALLER_DIR)/mercury.ini
	sed -i 's/sound_system = auto/sound_system = wasapi/g' $(WINDOWS_INSTALLER_DIR)/mercury.ini
	if ls $(HAMLIB_W64_DIR)/bin/*.dll >/dev/null 2>&1; then \
		cp $(HAMLIB_W64_DIR)/bin/*.dll $(WINDOWS_INSTALLER_DIR)/; \
	fi
	@echo "windows-installer staged."

windows-installer: windows fyne-ui-windows windows-installer-stage
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
	rm -f $(MACOS_DMG_UNIVERSAL) $(FYNE_UI_DIR)/$(MACOS_DMG_UNIVERSAL)
	@# NOT $(MACOS_CLI_PARK): the dmg recipe parks the CLI there precisely so it
	@# survives the `clean` that fyne-ui-macos-universal runs between its arch
	@# slices.  Cleaning it here deletes the binary mid-build.  The recipe removes
	@# it itself on success, and clears it before starting so a leftover from a
	@# failed run can never be staged as if it were fresh.
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
