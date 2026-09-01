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
LDFLAGS = -lpthread -lrt -lm $(SAN_LDFLAGS) $(ATOMIC_LDFLAGS)
endif

# Shared mock stubs for ARQ tests
ARQ_STUBS = datalink_arq/arq_test_stubs.c

# Test executables
TEST_BINS = test_ring_buffer test_arq_protocol test_arq_timing test_arq_fsm \
            test_tcp_interfaces test_resampler test_arq_ladder test_arq_sim \
            test_arq_conn test_cfg_utils test_arq_tnc test_channel_busy \
            test_mfsk test_watterson test_mfsk_modem test_pattern_ack_detection \
            test_freedv_harq test_pcm24 test_sock_wire test_virtual_clock \
            test_mercury_prng \
            test_ofdm_acq test_ui_status test_ui_devices test_tx_pacing \
            test_radio_port test_radio_io_frequency

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

# 24-bit packed PCM conversion test (offline, no sound card)
test_pcm24: audioio/test_pcm24.c $(UNITY_SRC)
	$(CC) $(CFLAGS) -I../audioio -o $@ $^ $(LDFLAGS) -lm

# ARQ protocol tests (frame encode/decode, utilities)
test_arq_protocol: datalink_arq/test_arq_protocol.c $(UNITY_SRC) $(ARQ_STUBS) \
                   ../common/virtual_clock.c ../datalink_arq/arq_protocol.c ../datalink_arq/arith.c
	$(CC) $(CFLAGS) -I../modem/freedv -o $@ $^ $(LDFLAGS)

# Delivery-driven ladder test (deterministic climb/drop, MFSK start, BW clamp)
test_arq_ladder: datalink_arq/test_arq_ladder.c $(UNITY_SRC) $(ARQ_STUBS) \
               ../datalink_arq/arq_fsm.c ../common/virtual_clock.c \
               ../datalink_arq/arq_protocol.c ../datalink_arq/arq_timing.c \
               ../datalink_arq/arith.c
	$(CC) $(CFLAGS) -I../modem/freedv -o $@ $^ $(LDFLAGS) -lm

# ARQ timing tests (instrumentation recording)
test_arq_timing: datalink_arq/test_arq_timing.c $(UNITY_SRC) $(ARQ_STUBS) \
                 ../datalink_arq/arq_timing.c ../common/virtual_clock.c \
                 ../datalink_arq/arq_protocol.c \
                 ../datalink_arq/arith.c
	$(CC) $(CFLAGS) -I../modem/freedv -o $@ $^ $(LDFLAGS)

# ARQ FSM tests (state transitions, callback invocations)
test_arq_fsm: datalink_arq/test_arq_fsm.c $(UNITY_SRC) $(ARQ_STUBS) \
              ../datalink_arq/arq_fsm.c ../common/virtual_clock.c \
              ../datalink_arq/arq_protocol.c ../datalink_arq/arq_timing.c \
              ../datalink_arq/arith.c
	$(CC) $(CFLAGS) -I../modem/freedv -o $@ $^ $(LDFLAGS)

# TCP TNC interface tests (command parsing, status emitters, broadcast framing)
# Uses #include "tcp_interfaces.c" — framer.c provides write_frame_header
# arq_tnc.c needed because interfaces_init() calls arq_set_tnc_callbacks()
# ../data_interfaces/tcp_interfaces.c is a PREREQUISITE, not a link argument:
# the test #includes it directly (see the top of the .c), so without it here
# make does not relink when the implementation changes and the suite silently
# re-runs the previous binary -- which is exactly how a mutation check passes
# when it should fail.  It stays off the link line ($^ would compile it twice).
test_tcp_interfaces: data_interfaces/test_tcp_interfaces.c $(UNITY_SRC) ../modem/framer.c \
                     ../datalink_arq/arq_tnc.c ../data_interfaces/tcp_interfaces.c
	$(CC) $(CFLAGS) -I../modem/freedv -o $@ \
	    data_interfaces/test_tcp_interfaces.c $(UNITY_SRC) ../modem/framer.c \
	    ../datalink_arq/arq_tnc.c $(LDFLAGS)

# Two-FSM ARQ simulation harness
SIM_SRC = sim/sim_clock.c sim/sim_channel.c sim/sim_endpoint.c \
          sim/sim_translate.c sim/sim_core.c sim/sim_props.c
test_arq_sim: sim/test_arq_sim.c $(UNITY_SRC) $(ARQ_STUBS) $(SIM_SRC) \
              ../datalink_arq/arq_fsm.c ../common/virtual_clock.c \
              ../datalink_arq/arq_protocol.c \
              ../datalink_arq/arq_timing.c ../datalink_arq/arith.c
	$(CC) $(CFLAGS) -Isim -I../modem/freedv -o $@ $^ $(LDFLAGS)

# arq_conn accessor correctness tests (single-threaded; locking verified by TSan)
test_arq_conn: datalink_arq/test_arq_conn.c $(UNITY_SRC) \
               datalink_arq/arq_conn_accessors.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Connect-latency instrument (not a pass/fail test — run it by hand).
# Mirrors test_arq_sim's link line, minus Unity.
connect_bench: sim/connect_bench.c $(ARQ_STUBS) $(SIM_SRC) \
               ../datalink_arq/arq_fsm.c ../common/virtual_clock.c \
               ../datalink_arq/arq_protocol.c \
               ../datalink_arq/arq_timing.c ../datalink_arq/arith.c
	$(CC) $(CFLAGS) -Isim -I../modem/freedv -o $@ $^ $(LDFLAGS)

test_cfg_utils: common/test_cfg_utils.c $(UNITY_SRC) \
                ../common/cfg_utils.c ../radio_io/cm108_ptt.c \
                ../common/hermes_log.c \
                ../common/iniparser/iniparser.c ../common/iniparser/dictionary.c
	$(CC) $(CFLAGS) -I../radio_io -o $@ $^ $(LDFLAGS)

# ARQ→TNC seam test (standalone TU, no threads/bus)
test_arq_tnc: datalink_arq/test_arq_tnc.c $(UNITY_SRC) ../datalink_arq/arq_tnc.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Channel-busy classifier test (pure state machine, virtual clock)
test_channel_busy: modem/test_channel_busy.c $(UNITY_SRC) ../modem/channel_busy.c
	$(CC) $(CFLAGS) -I../modem -o $@ $^ $(LDFLAGS) -lm

# HARQ combined-decode acceptance gate (drives the real DATAC15 LDPC decoder)
test_freedv_harq: modem/test_freedv_harq.c $(UNITY_SRC) \
                  ../modem/freedv/mpdecode_core.c ../modem/freedv/phi0.c \
                  ../modem/freedv/H_256_768_22.c
	$(CC) $(CFLAGS) -I../modem/freedv -o $@ $^ $(LDFLAGS) -lm
# -x sock wire codec: byte-exact pin of the lockstep frame layout
test_sock_wire: audioio/test_sock_wire.c $(UNITY_SRC)
	$(CC) $(CFLAGS) -I../audioio -o $@ $^ $(LDFLAGS)

# Virtual clock (time base for -x sock); hermes_uptime_ms stubbed in-test
test_virtual_clock: common/test_virtual_clock.c $(UNITY_SRC) ../common/virtual_clock.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Channel model: pins that fading stays finite across sample rates (-I.. because
# the test includes common/watterson.h by path).
test_watterson: common/test_watterson.c $(UNITY_SRC) ../common/watterson.c ../common/mercury_prng.c
	$(CC) $(CFLAGS) -I.. -o $@ $^ $(LDFLAGS)

# The channel's random source.  Pins actual output values, because the whole
# point is that a seed means the same fade on every platform.
test_mercury_prng: common/test_mercury_prng.c $(UNITY_SRC) ../common/mercury_prng.c
	$(CC) $(CFLAGS) -I.. -o $@ $^ $(LDFLAGS)

# OFDM acquisition: the FFT search must agree with brute force, fringe included.
# libfreedvdata.a is a PREREQUISITE, not just a link argument: leave it off the
# left-hand side and make will not relink when the library changes, which
# silently tests the previous build.
FREEDV_LIB = ../modem/freedv/libfreedvdata.a

# ...and it needs a rule, because CI runs `make -C tests test` on a clean tree
# without building the main project first.  Without this, the prerequisite above
# is unbuildable there ("No rule to make target") even though it resolves on a
# developer box that has already run the top-level make.
.PHONY: $(FREEDV_LIB)
$(FREEDV_LIB):
	$(MAKE) -C ../modem/freedv

test_ofdm_acq: modem/test_ofdm_acq.c $(UNITY_SRC) $(FREEDV_LIB)
	$(CC) $(CFLAGS) -I../modem/freedv -o $@ modem/test_ofdm_acq.c $(UNITY_SRC) $(FREEDV_LIB) $(LDFLAGS) -lm

# TX burst pacing (pure header; simulated coarse timer, no sleeping).
# tx_pacing.h is a PREREQUISITE: the logic under test lives entirely in the
# header, so without it make will not relink and the mutation check silently
# re-runs the previous binary.
test_tx_pacing: modem/test_tx_pacing.c $(UNITY_SRC) ../modem/tx_pacing.h
	$(CC) $(CFLAGS) -I../modem -o $@ modem/test_tx_pacing.c $(UNITY_SRC) $(LDFLAGS)

# radio_device vs the rig's port type (pure header; no hamlib needed).
# radio_port.h is a PREREQUISITE -- the logic under test lives in the header.
test_radio_port: radio_io/test_radio_port.c $(UNITY_SRC) ../radio_io/radio_port.h
	$(CC) $(CFLAGS) -I../radio_io -o $@ radio_io/test_radio_port.c $(UNITY_SRC) $(LDFLAGS)

# Optional CAT telemetry must return its cache immediately while PTT or another
# timing-sensitive radio operation owns the shared mutex.
test_radio_io_frequency: radio_io/test_radio_io_frequency.c $(UNITY_SRC) \
                         ../radio_io/radio_io.c ../common/hermes_log.c
	$(CC) $(CFLAGS) -DRADIO_IO_TEST -o $@ $^ $(LDFLAGS)

# UI status wire format (embedded struct vs remote JSON must not drift)
# ui_status.h is a PREREQUISITE: the device-id widths under test are #defines
# in the header, so without it make will not rebuild and the test silently
# re-runs the previous binary (which is exactly how a mutation check passes
# when it should fail).
test_ui_status: gui_interface/test_ui_status.c $(UNITY_SRC) ../gui_interface/ui_status.c ../gui_interface/ui_status.h
	$(CC) $(CFLAGS) -I../gui_interface -I../datalink_arq -I../modem -I../common -o $@ \
	    gui_interface/test_ui_status.c $(UNITY_SRC) ../gui_interface/ui_status.c $(LDFLAGS)

# UI device labelling (issue #189: two identical-looking USB codecs).
# ui_communication.h/ui_status.h are PREREQUISITES: the behaviour under test is
# in ui_devices.c but the types are in the headers, and without them make will
# not relink and a mutation check silently re-runs the previous binary.
test_ui_devices: gui_interface/test_ui_devices.c $(UNITY_SRC) \
                 ../gui_interface/ui_devices.c ../gui_interface/ui_communication.h \
                 ../gui_interface/ui_status.h
	$(CC) $(CFLAGS) -I../gui_interface -I../datalink_arq -I../modem -I../common -o $@ \
	    gui_interface/test_ui_devices.c $(UNITY_SRC) ../gui_interface/ui_devices.c $(LDFLAGS)

# MFSK codec test (mod/demod round-trip, modulation-order gain, OFDM framing)
test_mfsk: modem/test_mfsk.c $(UNITY_SRC) ../modem/mfsk.c ../modem/mfsk_ofdm.c ../modem/mfsk_sync.c ../modem/mfsk_ldpc.c ../modem/mfsk_ldpc_1_16.c ../modem/mfsk_ldpc_2_16.c ../modem/mfsk_ldpc_3_16.c ../modem/mfsk_ldpc_5_16.c ../modem/mfsk_ldpc_8_16.c
	$(CC) $(CFLAGS) -I../modem -o $@ $^ $(LDFLAGS) -lm

# MFSK modem backend (burst TX/RX through the vtable); links libfreedvdata for CRC16
MFSK_SRC = ../modem/mfsk.c ../modem/mfsk_ofdm.c ../modem/mfsk_sync.c ../modem/mfsk_ldpc.c \
           ../modem/mfsk_ldpc_1_16.c ../modem/mfsk_ldpc_2_16.c ../modem/mfsk_ldpc_3_16.c \
           ../modem/mfsk_ldpc_5_16.c ../modem/mfsk_ldpc_8_16.c

test_mfsk_modem: modem/test_mfsk_modem.c $(UNITY_SRC) ../modem/modem_mfsk.c $(MFSK_SRC) ../modem/freedv/libfreedvdata.a
	$(CC) $(CFLAGS) -I../modem -I../modem/freedv -o $@ $^ $(LDFLAGS) -lm

# Pattern ACK DSP test (mfsk_pattern_tx/detect: noise rejects, real detects)
test_pattern_ack_detection: modem/test_pattern_ack_detection.c $(UNITY_SRC) \
           ../modem/modem_mfsk.c $(MFSK_SRC)
	$(CC) $(CFLAGS) -I../modem -I../modem/freedv -o $@ $^ ../modem/freedv/libfreedvdata.a $(LDFLAGS) -lm

# The benches are not part of `all` (they are instruments, not pass/fail gates),
# but they MUST be removed by clean.  Leaving connect_bench behind meant a
# `make clean && make` cycle silently served a binary built from the previous
# tree -- an A/B that read byte-identical before and after a change that had in
# fact moved the number by 3.1 s.
BENCH_BINS = connect_bench

clean:
	rm -f $(TEST_BINS) $(BENCH_BINS)
