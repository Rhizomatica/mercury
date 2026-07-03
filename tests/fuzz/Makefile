# Fuzz-target build for Mercury's untrusted-byte parsers.
#
# Copyright (C) 2026 Rhizomatica
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Requires clang with libFuzzer. gcc (the tree default) cannot build these.
#   make -C tests/fuzz            # build all targets
#   make -C tests/fuzz check      # build + short smoke run over seed corpora
#
# Override the compiler if clang is not named "clang":
#   make -C tests/fuzz CC=clang-18

# clang is mandatory here regardless of the tree's default CC.
# On macOS, Apple's clang (from Command Line Tools) ships without the fuzzer
# runtime (libclang_rt.fuzzer_osx.a). The Homebrew LLVM package provides it.
# Auto-detect: prefer the Homebrew LLVM clang if the system clang lacks it.
ifeq ($(origin FUZZ_CC),undefined)
  _BREW_CLANG := /opt/homebrew/opt/llvm/bin/clang
  ifneq ($(wildcard $(_BREW_CLANG)),)
    FUZZ_CC := $(_BREW_CLANG)
  else
    FUZZ_CC := clang
  endif
endif

# Base flags mirror ../../config.mk, plus the fuzzer + sanitizers.
FUZZ_SAN     = -fsanitize=fuzzer,address,undefined
FUZZ_CFLAGS  = -Wall -O1 -g -std=gnu11 -D_GNU_SOURCE $(FUZZ_SAN) \
                -ffunction-sections
FUZZ_LDFLAGS = $(FUZZ_SAN) -lm -lpthread

PROJECT_INC = -I../../common -I../../modem -I../../datalink_arq \
              -I../../datalink_broadcast -I../../data_interfaces \
              -I../../gui_interface -I../../radio_io \
              -I../../audioio/ffaudio -I../../modem/freedv

# Populated by later tasks.
FUZZ_BINS = fuzz_arq_hdr fuzz_frame_header fuzz_kiss fuzz_arith

# Number of iterations for the smoke `check` run (kept short: this is a
# build+no-crash gate, not a fuzzing campaign).
CHECK_RUNS ?= 20000

.PHONY: all check preflight clean
all: preflight $(FUZZ_BINS)

# Fail loudly and early if the chosen compiler cannot do libFuzzer.
preflight:
	@echo 'int LLVMFuzzerTestOneInput(const unsigned char*b,unsigned long n){(void)b;(void)n;return 0;}' > .preflight.c
	@if ! $(FUZZ_CC) $(FUZZ_SAN) .preflight.c -o .preflight.bin >/dev/null 2>&1; then \
	  rm -f .preflight.c .preflight.bin; \
	  echo "ERROR: $(FUZZ_CC) cannot build -fsanitize=fuzzer targets."; \
	  echo "  On macOS: brew install llvm, then retry (auto-detected via /opt/homebrew/opt/llvm/bin/clang)."; \
	  echo "  Or pass FUZZ_CC=/path/to/clang to override."; \
	  exit 1; \
	fi
	@rm -f .preflight.c .preflight.bin
	@echo "libFuzzer preflight OK ($(FUZZ_CC))"

check: all
	@fail=0; \
	for t in $(FUZZ_BINS); do \
	  echo "--- smoke-fuzzing $$t ($(CHECK_RUNS) runs) ---"; \
	  ./$$t -runs=$(CHECK_RUNS) corpus/$$t || fail=1; \
	  if [ -n "$$(ls -A regressions/$$t 2>/dev/null | grep -v '^.gitkeep$$')" ]; then \
	    echo "--- replaying saved regressions for $$t ---"; \
	    for f in regressions/$$t/*; do \
	      [ "$$(basename $$f)" = ".gitkeep" ] && continue; \
	      ./$$t "$$f" || fail=1; \
	    done; \
	  fi; \
	done; \
	if [ $$fail -ne 0 ]; then echo "FUZZ SMOKE FAILED"; exit 1; fi; \
	printf '\n=== All fuzz smoke runs passed ===\n\n'

clean:
	rm -f $(FUZZ_BINS) .preflight.c .preflight.bin crash-* leak-* oom-* timeout-*

fuzz_arq_hdr: fuzz_arq_hdr.c ../../datalink_arq/arq_protocol.c \
              ../../datalink_arq/arith.c ../../modem/framer.c
	$(FUZZ_CC) $(FUZZ_CFLAGS) $(PROJECT_INC) -o $@ $^ $(FUZZ_LDFLAGS)

fuzz_frame_header: fuzz_frame_header.c ../../modem/framer.c
	$(FUZZ_CC) $(FUZZ_CFLAGS) $(PROJECT_INC) -o $@ $^ $(FUZZ_LDFLAGS)

fuzz_kiss: fuzz_kiss.c ../../datalink_broadcast/kiss.c
	$(FUZZ_CC) $(FUZZ_CFLAGS) $(PROJECT_INC) -o $@ $^ $(FUZZ_LDFLAGS)

fuzz_arith: fuzz_arith.c ../../datalink_arq/arith.c
	$(FUZZ_CC) $(FUZZ_CFLAGS) $(PROJECT_INC) -o $@ $^ $(FUZZ_LDFLAGS)
