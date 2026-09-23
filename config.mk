# config.mk - Shared build configuration for HERMES Modem
#
# All Makefiles include this file to get consistent CC, AR, and base CFLAGS.
# Each subdirectory appends its own local include paths and extra flags.
#
# Override on the command line for cross-compilation, e.g.:
#   make CC=arm-linux-gnueabihf-gcc
#   make CC=clang COMMON_CFLAGS="-Wall -O2 -std=gnu11 -march=armv7-a"
#
# Platform-specific tuning (detected from the compiler target for aarch64,
# or set PLATFORM=):
#   make PLATFORM=rpi4          # Raspberry Pi 4 (Cortex-A72)
#   make PLATFORM=rpi5          # Raspberry Pi 5 (Cortex-A76)

ifeq ($(origin CC),default)
CC = gcc
endif

ifeq ($(origin AR),default)
AR = ar
endif

EXTRA_CFLAGS := $(CFLAGS)
COMMON_CFLAGS ?= -Wall -O2 -std=gnu11 -pthread -D_GNU_SOURCE
COMMON_CFLAGS += $(EXTRA_CFLAGS)

# Header dependency tracking.
#
# Without this, editing a header does NOT rebuild the objects that include it,
# and make happily links a binary from a mix of old and new layouts.  Adding a
# field to arq_session_t in arq_fsm.h and rebuilding incrementally produced a
# mercury whose ARQ event loop read deadline_ms at the wrong offset: it started
# a CALL and then sat forever, looking exactly like a protocol bug.  Two
# separate measurements were built on such binaries and had to be thrown away.
#
# -MMD writes a .d beside each .o listing the headers it used; -MP adds a
# phony target for each header so a DELETED or renamed header does not wedge
# the build with "no rule to make target".  Each Makefile then -includes the
# .d files next to its own objects (paths in a .d are relative to the
# directory the compiler ran in, so they must be included from there).
COMMON_CFLAGS += -MMD -MP

# Rebuild objects when the build configuration changes.
#
# -MMD tracks headers, not flags: an object depends on its source and headers
# only, so changing a compile flag -- here or in the Makefile that compiles it
# -- leaves every existing object "up to date".  5ea3356 added a packed-web
# compile flag for the old vendored web server together with a generated
# web_packed.o, in a Makefile-only change.  An incremental build of an older
# tree kept a library object that still carried the library's own stub
# versions of the unpack hooks, linked it beside the generated ones, and
# failed with "multiple definition".  The same trap applies to WS_TLS_CFLAGS
# below: it decides whether ws_tls.o has an OpenSSL backend at all.
#
# BUILD_CONFIG is the including Makefile plus this file (MAKEFILE_LIST as it
# stands here).  Each Makefile makes the objects it compiles depend on it, next
# to its -include of .d files.  Only objects already on disk are named: one
# that does not exist yet gets built regardless.  Variables given on the
# command line (make CC=..., OS=Windows_NT) are still not tracked.
BUILD_CONFIG := $(MAKEFILE_LIST)

# Detect the compiler target so cross-compiling armhf from an aarch64 host
# doesn't inherit aarch64-only flags from uname -m.
CC_MACHINE := $(strip $(shell $(CC) -dumpmachine 2>/dev/null))
TARGET_MACHINE := $(if $(CC_MACHINE),$(CC_MACHINE),$(shell uname -m 2>/dev/null))

# --- Platform-specific flags for aarch64 targets ---
ifneq ($(filter aarch64%,$(TARGET_MACHINE)),)
  # Runtime-dispatched atomics: uses LSE on ARMv8.1+ cores, falls back to
  # LL/SC on older cores.  Safe and correct on all aarch64 (GCC 10+).
  COMMON_CFLAGS += -moutline-atomics

  ifeq ($(PLATFORM),rpi4)
    COMMON_CFLAGS += -mcpu=cortex-a72
  else ifeq ($(PLATFORM),rpi5)
    COMMON_CFLAGS += -mcpu=cortex-a76
  else ifndef PLATFORM
    # Auto-detect Raspberry Pi model from device tree
    _PI_MODEL := $(shell cat /sys/firmware/devicetree/base/model 2>/dev/null)
    ifneq ($(findstring Raspberry Pi 4,$(_PI_MODEL)),)
      COMMON_CFLAGS += -mcpu=cortex-a72
    else ifneq ($(findstring Raspberry Pi 5,$(_PI_MODEL)),)
      COMMON_CFLAGS += -mcpu=cortex-a76
    endif
  endif
endif

# 32-bit ARM (armhf / armv7) has no native 64-bit atomic instructions on the
# baseline this ships for, so GCC lowers 64-bit _Atomic loads/stores to
# libatomic calls (__atomic_load_8, __atomic_store_8, ...).  Link libatomic
# there.  64-bit targets inline these, and libatomic is absent on some
# platforms (e.g. macOS), so keep it scoped.
#
# Match arm% rather than the narrower "arm-% armv%": the value here is normally
# $(CC) -dumpmachine (arm-linux-gnueabihf), but anyone who overrides it by hand
# reaches for the name of the port -- "armhf" -- which matched NEITHER this rule
# nor the aarch64 one above, silently dropping -latomic on the one target that
# needs it (issue #200).  Exclude arm64 explicitly: Apple Clang reports targets
# such as arm64-apple-darwin, and macOS does not provide a separate libatomic.
ATOMIC_LDFLAGS =
ifneq ($(filter arm%,$(TARGET_MACHINE)),)
ifeq ($(filter arm64%,$(TARGET_MACHINE)),)
  ATOMIC_LDFLAGS = -latomic
endif
ifneq ($(filter m68k%,$(TARGET_MACHINE)),)
  ATOMIC_LDFLAGS = -latomic
endif
endif

# --- Optional TLS (wss://) for the UI web server ---
#
# The built-in WebSocket server can serve wss:// through OpenSSL, which is
# Apache-2.0 and so compatible with Mercury's GPL-3.0-or-later.  Default is to
# USE IT WHEREVER THE SYSTEM HAS IT -- detected with pkg-config, the same way
# hamlib and hidapi are.  A build without it still works: the server serves
# plain ws:// and refuses wss:// at startup with a clear message, rather than
# failing per connection.
#
# Windows is the one exception, and not by preference: that build is a mingw
# cross-compile, where a bare `pkg-config --exists openssl` answers for the
# HOST.  Believing it would link an ELF shared library into a PE, so it is
# skipped unless someone points PKG_CONFIG_LIBDIR at a real mingw OpenSSL and
# asks for it with WS_TLS=1.
#
# That exception is a "not yet", not a "never": the way to give Windows and
# universal macOS wss:// is to vendor a static OpenSSL the way radio_io already
# vendors hamlib-w64 and the fat hamlib-macos, and then drop the guard below.
#
#   WS_TLS=0   never use OpenSSL
#   WS_TLS=1   use it if pkg-config finds it (default), Windows included
WS_TLS ?= 1
WS_TLS_CFLAGS :=
WS_TLS_LDFLAGS :=

WS_TLS_CROSS_SAFE := 1
ifeq ($(OS),Windows_NT)
ifneq ($(origin WS_TLS),command line)
  WS_TLS_CROSS_SAFE := 0
endif
endif

ifeq ($(WS_TLS),1)
ifeq ($(WS_TLS_CROSS_SAFE),1)
  WS_OPENSSL_OK := $(shell pkg-config --exists openssl 2>/dev/null && echo 1)
  ifeq ($(WS_OPENSSL_OK),1)
    WS_TLS_CFLAGS := -DWS_HAVE_OPENSSL=1 $(shell pkg-config --cflags openssl)
    WS_TLS_LDFLAGS := $(shell pkg-config --libs openssl)
  endif
endif
endif

# --- Optional ARQ session encryption (libsodium) ---
#
# [crypto] mode = optional | required needs libsodium (ISC licence, GPL-
# compatible), for X25519, ChaCha20-Poly1305 and SHA-256.  Same rule as TLS
# above: used wherever pkg-config finds it, with the mingw cross-build the one
# exception, for the same host-versus-target reason.  Without it Mercury still
# builds and runs with crypto off, and refuses mode != off at startup rather
# than silently sending in the clear.
#
# The define goes into COMMON_CFLAGS so that every translation unit agrees on
# it.  arq_xs_t has the same layout either way, but anything compiled from
# arq_crypto.c or noise_kk.c -- the daemon and the tests alike -- must see
# the same answer to "is there a backend".
#
#   ARQ_CRYPTO=0   never use libsodium
#   ARQ_CRYPTO=1   use it if pkg-config finds it (default)
ARQ_CRYPTO ?= 1
CRYPTO_CFLAGS :=
CRYPTO_LDFLAGS :=

ARQ_CRYPTO_CROSS_SAFE := 1
ifeq ($(OS),Windows_NT)
ifneq ($(origin ARQ_CRYPTO),command line)
  ARQ_CRYPTO_CROSS_SAFE := 0
endif
endif

ifeq ($(ARQ_CRYPTO),1)
ifeq ($(ARQ_CRYPTO_CROSS_SAFE),1)
  SODIUM_OK := $(shell pkg-config --exists libsodium 2>/dev/null && echo 1)
  ifeq ($(SODIUM_OK),1)
    CRYPTO_CFLAGS := -DARQ_HAVE_CRYPTO $(shell pkg-config --cflags libsodium)
    CRYPTO_LDFLAGS := $(shell pkg-config --libs libsodium)
  endif
endif
endif
COMMON_CFLAGS += $(CRYPTO_CFLAGS)

GIT_HASH ?= $(shell git rev-parse --short=8 HEAD 2>/dev/null || echo unknown000)
COMMON_CFLAGS += -DGIT_HASH=\"$(GIT_HASH)\"

# Optional: enable DIAG I/O debug logging (default: off)
# Enable with: make DEBUG_IO=1
DEBUG_IO ?= 0
ifeq ($(DEBUG_IO),1)
  COMMON_CFLAGS += -DDEBUG_IO
endif

# Optional: build with sanitizers (default: off). Mutually exclusive.
# Enable with:  make SANITIZE_TSAN=1 ...        (ThreadSanitizer)
#          or:  make SANITIZE_ASAN_UBSAN=1 ...  (Address + UndefinedBehavior)
# Sanitizer flags must reach the linker too, so they are also collected into
# SAN_LDFLAGS, which every link line appends (the binary link uses LDFLAGS,
# not CFLAGS, so COMMON_CFLAGS alone would not instrument the final link).
SAN_LDFLAGS =

SANITIZE_TSAN ?= 0
SANITIZE_ASAN_UBSAN ?= 0

ifeq ($(SANITIZE_TSAN),1)
  ifeq ($(SANITIZE_ASAN_UBSAN),1)
    $(error SANITIZE_TSAN and SANITIZE_ASAN_UBSAN are mutually exclusive)
  endif
  SAN_FLAGS := -fsanitize=thread -g -O1 -fno-omit-frame-pointer
  COMMON_CFLAGS += $(SAN_FLAGS)
  SAN_LDFLAGS += -fsanitize=thread
endif

ifeq ($(SANITIZE_ASAN_UBSAN),1)
  SAN_FLAGS := -fsanitize=address,undefined -fno-sanitize-recover=undefined \
               -g -O1 -fno-omit-frame-pointer
  COMMON_CFLAGS += $(SAN_FLAGS)
  SAN_LDFLAGS += -fsanitize=address,undefined
endif
