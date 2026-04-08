# config.mk - Shared build configuration for HERMES Modem
#
# All Makefiles include this file to get consistent CC, AR, and base CFLAGS.
# Each subdirectory appends its own local include paths and extra flags.
#
# Override on the command line for cross-compilation, e.g.:
#   make CC=arm-linux-gnueabihf-gcc
#   make CC=clang COMMON_CFLAGS="-Wall -O2 -std=gnu11 -march=armv7-a"
#
# Platform-specific tuning (auto-detected for aarch64, or set PLATFORM=):
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

# --- Platform-specific flags for aarch64 ---
UNAME_M := $(shell uname -m 2>/dev/null)
ifeq ($(UNAME_M),aarch64)
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

GIT_HASH ?= $(shell git rev-parse --short=8 HEAD 2>/dev/null || echo unknown000)
COMMON_CFLAGS += -DGIT_HASH=\"$(GIT_HASH)\"

# Optional: enable DIAG I/O debug logging (default: off)
# Enable with: make DEBUG_IO=1
DEBUG_IO ?= 0
ifeq ($(DEBUG_IO),1)
  COMMON_CFLAGS += -DDEBUG_IO
endif
