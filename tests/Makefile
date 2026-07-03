# Makefile for WebSocket-based UI communication
#
# Copyright (C) 2026 Rhizomatica
# Author: Pedro Messetti <pedromessetti.rhizomatica@gmail.com>
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

# Test framework
UNITY_SRC = unity/unity.c
UNITY_INC = -Iunity -Ifff
UNITY_CFLAGS = -DUNITY_SUPPORT_64

# Project includes
PROJECT_INC = -I../common -I../modem -I../datalink_arq -I../datalink_broadcast \
              -I../data_interfaces -I../gui_interface -I../radio_io -I../audioio/ffaudio

CFLAGS = $(COMMON_CFLAGS) $(UNITY_CFLAGS) $(UNITY_INC) $(PROJECT_INC)
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
LDFLAGS = -lpthread -lm $(SAN_LDFLAGS)
else
LDFLAGS = -lpthread -lrt -lm $(SAN_LDFLAGS)
endif

# Shared mock stubs for ARQ tests
ARQ_STUBS = datalink_arq/arq_test_stubs.c

# Test executables
TEST_BINS = test_ring_buffer test_arq_protocol test_arq_timing test_arq_fsm \
            test_tcp_interfaces test_resampler test_arq_olla test_arq_sim

.PHONY: all test clean

all: test

test: $(TEST_BINS)
	@echo "\n=== Running Unit Tests ===\n"
	@fail=0; \
	for t in $(TEST_BINS); do \
		echo "--- Running $$t ---"; \
		./$$t || fail=1; \
	done; \
	if [ $$fail -ne 0 ]; then echo "TESTS FAILED"; exit 1; fi
	@echo "\n=== All Tests Passed ===\n"

# Common module tests
test_ring_buffer: common/test_ring_buffer.c $(UNITY_SRC) ../common/ring_buffer_posix.c ../common/os_interop.c ../common/shm_posix.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Audio resampler test (offline, no sound card)
test_resampler: audioio/test_resampler.c $(UNITY_SRC) ../audioio/resampler.c
	$(CC) $(CFLAGS) -I../audioio -o $@ $^ $(LDFLAGS) -lm

# ARQ protocol tests (frame encode/decode, utilities)
test_arq_protocol: datalink_arq/test_arq_protocol.c $(UNITY_SRC) $(ARQ_STUBS) \
                   ../datalink_arq/arq_protocol.c ../datalink_arq/arith.c
	$(CC) $(CFLAGS) -I../modem/freedv -o $@ $^ $(LDFLAGS)

# OLLA control-loop test (closed-loop gear-shift convergence)
test_arq_olla: datalink_arq/test_arq_olla.c $(UNITY_SRC) $(ARQ_STUBS) \
               ../datalink_arq/arq_protocol.c ../datalink_arq/arith.c
	$(CC) $(CFLAGS) -I../modem/freedv -o $@ $^ $(LDFLAGS) -lm

# ARQ timing tests (instrumentation recording)
test_arq_timing: datalink_arq/test_arq_timing.c $(UNITY_SRC) $(ARQ_STUBS) \
                 ../datalink_arq/arq_timing.c ../datalink_arq/arq_protocol.c \
                 ../datalink_arq/arith.c
	$(CC) $(CFLAGS) -I../modem/freedv -o $@ $^ $(LDFLAGS)

# ARQ FSM tests (state transitions, callback invocations)
test_arq_fsm: datalink_arq/test_arq_fsm.c $(UNITY_SRC) $(ARQ_STUBS) \
              ../datalink_arq/arq_fsm.c \
              ../datalink_arq/arq_protocol.c ../datalink_arq/arq_timing.c \
              ../datalink_arq/arith.c
	$(CC) $(CFLAGS) -I../modem/freedv -o $@ $^ $(LDFLAGS)

# TCP TNC interface tests (command parsing, status emitters, broadcast framing)
# Uses #include "tcp_interfaces.c" — framer.c provides write_frame_header
test_tcp_interfaces: data_interfaces/test_tcp_interfaces.c $(UNITY_SRC) ../modem/framer.c
	$(CC) $(CFLAGS) -I../modem/freedv -o $@ $^ $(LDFLAGS)

# Two-FSM ARQ simulation harness
SIM_SRC = sim/sim_clock.c sim/sim_channel.c sim/sim_endpoint.c \
          sim/sim_translate.c sim/sim_core.c sim/sim_props.c
test_arq_sim: sim/test_arq_sim.c $(UNITY_SRC) $(ARQ_STUBS) $(SIM_SRC) \
              ../datalink_arq/arq_fsm.c ../datalink_arq/arq_protocol.c \
              ../datalink_arq/arq_timing.c ../datalink_arq/arith.c
	$(CC) $(CFLAGS) -Isim -I../modem/freedv -o $@ $^ $(LDFLAGS)

clean:
	rm -f $(TEST_BINS)
