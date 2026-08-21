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
endif

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
