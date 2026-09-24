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

# -MMD is stripped here on purpose, and there is no -include of .d files.
#
# Every rule below compiles and links in ONE step.  With several sources and a
# single -o, gcc writes a .d per source but names them all after the OUTPUT, so
# each overwrites the last: the surviving file says "test_foo: last_unit.c" and
# records no headers at all.  It buys nothing.
#
# It also actively broke the build.  Including such a .d makes its contents a
# prerequisite of the binary, and a bare $^ then handed a .h to the compiler as
# a source ("unknown type name 'uint8_t'" out of a perfectly good header).  It
# only bit on a REBUILD -- a clean checkout has no .d, so the first build
# succeeds and creates it and the second fails -- which is exactly why it got
# through CI.
#
# Real header tracking here would mean compiling to .o first; these binaries
# are cheap to rebuild, so they are simply rebuilt.  The $(filter) in each
# recipe stays as a guard in case a .d ever reappears.

# Test framework
UNITY_SRC = unity/unity.c
UNITY_INC = -Iunity -Ifff
UNITY_CFLAGS = -DUNITY_SUPPORT_64

# Project includes
PROJECT_INC = -I../common -I../modem -I../modem/mfsk -I../datalink_arq -I../datalink_broadcast \
              -I../data_interfaces -I../gui_interface -I../radio_io -I../audioio/ffaudio

CFLAGS = $(filter-out -MMD -MP,$(COMMON_CFLAGS)) $(UNITY_CFLAGS) $(UNITY_INC) $(PROJECT_INC)
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
            test_tcp_interfaces test_resampler test_pcm24 test_arq_olla test_arq_sim \
            test_arq_conn test_cfg_utils test_arq_tnc test_channel_busy \
            test_freedv_harq test_sock_wire test_virtual_clock test_watterson \
            test_mercury_prng test_bcast_file \
            test_ofdm_acq test_ui_status test_ui_devices test_ui_ptt_persist test_tx_pacing test_radio_port \
            test_radio_io_frequency test_mfsk test_rs_erasure \
			test_message_store test_ui_history test_ws_protocol

.PHONY: all test clean

all: test

# Rebuild the tests when their flags change (BUILD_CONFIG, see config.mk).  The
# recipes filter $^, so these never reach the compiler.
$(TEST_BINS): $(BUILD_CONFIG)

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
test_mfsk: modem/test_mfsk.c $(UNITY_SRC) ../modem/mfsk/mfsk.c ../modem/mfsk/mfsk_ofdm.c ../modem/mfsk/mfsk_sync.c ../modem/mfsk/mfsk_ldpc.c ../modem/mfsk/mfsk_ldpc_1_16.c ../modem/mfsk/mfsk_ldpc_2_16.c ../modem/mfsk/mfsk_ldpc_3_16.c ../modem/mfsk/mfsk_ldpc_5_16.c ../modem/mfsk/mfsk_ldpc_8_16.c
	$(CC) $(CFLAGS) -I../modem -I../modem/mfsk -o $@ $(filter %.c %.a %.o,$^) $(LDFLAGS) -lm

test_ring_buffer: common/test_ring_buffer.c $(UNITY_SRC) ../common/ring_buffer_posix.c ../common/os_interop.c ../common/shm_posix.c
	$(CC) $(CFLAGS) -o $@ $(filter %.c %.a %.o,$^) $(LDFLAGS)

# Audio resampler test (offline, no sound card)
test_resampler: audioio/test_resampler.c $(UNITY_SRC) ../audioio/resampler.c
	$(CC) $(CFLAGS) -I../audioio -o $@ $(filter %.c %.a %.o,$^) $(LDFLAGS) -lm

# 24-bit packed PCM conversion test (offline, no sound card)
test_pcm24: audioio/test_pcm24.c $(UNITY_SRC)
	$(CC) $(CFLAGS) -I../audioio -o $@ $(filter %.c %.a %.o,$^) $(LDFLAGS) -lm

# ARQ protocol tests (frame encode/decode, utilities)
test_arq_protocol: datalink_arq/test_arq_protocol.c $(UNITY_SRC) $(ARQ_STUBS) \
                   ../common/virtual_clock.c ../datalink_arq/arq_protocol.c ../datalink_arq/arith.c
	$(CC) $(CFLAGS) -I../modem/freedv -o $@ $(filter %.c %.a %.o,$^) $(LDFLAGS)

# OLLA control-loop test (closed-loop gear-shift convergence)
test_arq_olla: datalink_arq/test_arq_olla.c $(UNITY_SRC) $(ARQ_STUBS) \
               ../common/virtual_clock.c ../datalink_arq/arq_protocol.c ../datalink_arq/arith.c
	$(CC) $(CFLAGS) -I../modem/freedv -o $@ $(filter %.c %.a %.o,$^) $(LDFLAGS) -lm

# ARQ timing tests (instrumentation recording)
test_arq_timing: datalink_arq/test_arq_timing.c $(UNITY_SRC) $(ARQ_STUBS) \
                 ../datalink_arq/arq_timing.c ../common/virtual_clock.c \
                 ../datalink_arq/arq_protocol.c \
                 ../datalink_arq/arith.c
	$(CC) $(CFLAGS) -I../modem/freedv -o $@ $(filter %.c %.a %.o,$^) $(LDFLAGS)

# ARQ FSM tests (state transitions, callback invocations)
test_arq_fsm: datalink_arq/test_arq_fsm.c $(UNITY_SRC) $(ARQ_STUBS) \
              ../datalink_arq/arq_fsm.c ../common/virtual_clock.c \
              ../datalink_arq/arq_protocol.c ../datalink_arq/arq_timing.c \
              ../datalink_arq/arith.c
	$(CC) $(CFLAGS) -I../modem/freedv -o $@ $(filter %.c %.a %.o,$^) $(LDFLAGS)

# TCP TNC interface tests (command parsing, status emitters, broadcast framing)
# Uses #include "tcp_interfaces.c" — framer.c provides write_frame_header
# arq_tnc.c needed because interfaces_init() calls arq_set_tnc_callbacks()
# ../data_interfaces/tcp_interfaces.c is a PREREQUISITE, not a link argument:
# the test #includes it directly (see the top of the .c), so without it here
# make does not relink when the implementation changes and the suite silently
# re-runs the previous binary -- which is exactly how a mutation check passes
# when it should fail.  It stays off the link line ($(filter %.c %.a %.o,$^) would compile it twice).
# ../common/mercury_modes.c carries the -m/MODE index table; linking the real
# one (rather than a copy in the test) is the point -- the MODE tests assert on
# specific indexes, which is only meaningful against the shipping table.
test_tcp_interfaces: data_interfaces/test_tcp_interfaces.c $(UNITY_SRC) ../modem/framer.c \
                     ../datalink_arq/arq_tnc.c ../data_interfaces/tcp_interfaces.c \
                     ../common/ring_buffer_posix.c ../common/shm_posix.c ../common/os_interop.c \
                     ../common/mercury_modes.c
	$(CC) $(CFLAGS) -I../modem/freedv -o $@ \
	    data_interfaces/test_tcp_interfaces.c $(UNITY_SRC) ../modem/framer.c \
	    ../datalink_arq/arq_tnc.c ../common/shm_posix.c ../common/os_interop.c \
	    ../common/mercury_modes.c $(LDFLAGS)

# Reed-Solomon erasure code for the carousel ARQ prototype
test_rs_erasure: sim/test_rs_erasure.c sim/rs_erasure.c sim/rs_erasure.h $(UNITY_SRC)
	$(CC) $(CFLAGS) -Isim -o $@ $(filter %.c %.a %.o,$^) $(LDFLAGS)

# Prototype: erasure-coded carousel ARQ on the sim's channel (not run by `make
# test`; a bench, like ab_bench):  ./carousel_bench <seed> <channel> [bidir]
carousel_bench: sim/carousel_bench.c sim/rs_erasure.c sim/sim_channel.c $(ARQ_STUBS) \
                ../datalink_arq/arq_protocol.c ../datalink_arq/arith.c ../common/virtual_clock.c
	$(CC) $(CFLAGS) -Isim -I../modem/freedv -o $@ $(filter %.c %.a %.o,$^) $(LDFLAGS)

# Two-FSM ARQ simulation harness
SIM_SRC = sim/sim_clock.c sim/sim_channel.c sim/sim_endpoint.c \
          sim/sim_translate.c sim/sim_core.c sim/sim_props.c
test_arq_sim: sim/test_arq_sim.c $(UNITY_SRC) $(ARQ_STUBS) $(SIM_SRC) \
              ../datalink_arq/arq_fsm.c ../common/virtual_clock.c \
              ../datalink_arq/arq_protocol.c \
              ../datalink_arq/arq_timing.c ../datalink_arq/arith.c
	$(CC) $(CFLAGS) -Isim -I../modem/freedv -o $@ $(filter %.c %.a %.o,$^) $(LDFLAGS)

# Deterministic A/B bench (not part of `make test`):
#   SIM_CS=400 ./ab_bench <seed> <channel> [bidir]
ab_bench: sim/ab_bench.c $(ARQ_STUBS) $(SIM_SRC) \
          ../datalink_arq/arq_fsm.c ../common/virtual_clock.c \
          ../datalink_arq/arq_protocol.c \
          ../datalink_arq/arq_timing.c ../datalink_arq/arith.c
	$(CC) $(CFLAGS) -Isim -I../modem/freedv -o $@ $(filter %.c %.a %.o,$^) $(LDFLAGS)

# arq_conn accessor correctness tests (single-threaded; locking verified by TSan)
test_arq_conn: datalink_arq/test_arq_conn.c $(UNITY_SRC) \
               datalink_arq/arq_conn_accessors.c
	$(CC) $(CFLAGS) -o $@ $(filter %.c %.a %.o,$^) $(LDFLAGS)

test_cfg_utils: common/test_cfg_utils.c $(UNITY_SRC) \
                ../common/cfg_utils.c ../radio_io/cm108_ptt.c \
                ../common/hermes_log.c \
                ../common/iniparser/iniparser.c ../common/iniparser/dictionary.c
	$(CC) $(CFLAGS) -I../radio_io -o $@ $(filter %.c %.a %.o,$^) $(LDFLAGS)

# ARQ→TNC seam test (standalone TU, no threads/bus)
test_arq_tnc: datalink_arq/test_arq_tnc.c $(UNITY_SRC) ../datalink_arq/arq_tnc.c
	$(CC) $(CFLAGS) -o $@ $(filter %.c %.a %.o,$^) $(LDFLAGS)

# Channel-busy classifier test (pure state machine, virtual clock)
test_channel_busy: modem/test_channel_busy.c $(UNITY_SRC) ../modem/channel_busy.c
	$(CC) $(CFLAGS) -I../modem -I../modem/mfsk -o $@ $(filter %.c %.a %.o,$^) $(LDFLAGS) -lm

# HARQ combined-decode acceptance gate (drives the real DATAC15 LDPC decoder)
test_freedv_harq: modem/test_freedv_harq.c $(UNITY_SRC) \
                  ../modem/freedv/mpdecode_core.c ../modem/freedv/phi0.c \
                  ../modem/freedv/H_256_768_22.c
	$(CC) $(CFLAGS) -I../modem/freedv -o $@ $(filter %.c %.a %.o,$^) $(LDFLAGS) -lm
# -x sock wire codec: byte-exact pin of the lockstep frame layout
test_sock_wire: audioio/test_sock_wire.c $(UNITY_SRC)
	$(CC) $(CFLAGS) -I../audioio -o $@ $(filter %.c %.a %.o,$^) $(LDFLAGS)

# Virtual clock (time base for -x sock); hermes_uptime_ms stubbed in-test
test_virtual_clock: common/test_virtual_clock.c $(UNITY_SRC) ../common/virtual_clock.c
	$(CC) $(CFLAGS) -o $@ $(filter %.c %.a %.o,$^) $(LDFLAGS)

# Channel model: pins that fading stays finite across sample rates (-I.. because
# the test includes common/watterson.h by path).
test_watterson: common/test_watterson.c $(UNITY_SRC) ../common/watterson.c ../common/mercury_prng.c
	$(CC) $(CFLAGS) -I.. -o $@ $(filter %.c %.a %.o,$^) $(LDFLAGS)

# The channel's random source.  Pins actual output values, because the whole
# point is that a seed means the same fade on every platform.
test_mercury_prng: common/test_mercury_prng.c $(UNITY_SRC) ../common/mercury_prng.c
	$(CC) $(CFLAGS) -I.. -o $@ $(filter %.c %.a %.o,$^) $(LDFLAGS)

# Persistent message store (JSONL append, feed/append/count/get, reload).
test_message_store: common/test_message_store.c $(UNITY_SRC) ../common/message_store.c
	$(CC) $(CFLAGS) -o $@ common/test_message_store.c $(UNITY_SRC) ../common/message_store.c $(LDFLAGS)

# UI history frame builder (pure string function; no engine/websocket deps).
test_ui_history: gui_interface/test_ui_history.c $(UNITY_SRC) ../gui_interface/ui_history.c
	$(CC) $(CFLAGS) -I../gui_interface -o $@ gui_interface/test_ui_history.c $(UNITY_SRC) ../gui_interface/ui_history.c $(LDFLAGS)

# Broadcast file TX: the frames must decode in a hermes-broadcast receiver, so
# the test drives a full carousel through simulated loss and requires the file
# back byte-for-byte.
RAPTORQ_SRC = $(wildcard ../datalink_broadcast/raptorq/lib/*.c) \
              $(wildcard ../datalink_broadcast/raptorq/deps/obl/*.c)
test_bcast_file: datalink_broadcast/test_bcast_file.c $(UNITY_SRC) \
                 ../datalink_broadcast/bcast_file.c $(RAPTORQ_SRC)
	$(CC) $(CFLAGS) -I.. -I../datalink_broadcast -I../datalink_broadcast/raptorq/include \
	      -I../datalink_broadcast/raptorq/deps -o $@ $(filter %.c %.a %.o,$^) $(LDFLAGS) -lm

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
	$(CC) $(CFLAGS) -I../modem -I../modem/mfsk -o $@ modem/test_tx_pacing.c $(UNITY_SRC) $(LDFLAGS)

# radio_device vs the rig's port type (pure header; no hamlib needed).
# radio_port.h is a PREREQUISITE -- the logic under test lives in the header.
test_radio_port: radio_io/test_radio_port.c $(UNITY_SRC) ../radio_io/radio_port.h
	$(CC) $(CFLAGS) -I../radio_io -o $@ radio_io/test_radio_port.c $(UNITY_SRC) $(LDFLAGS)

# Optional CAT telemetry must return its cache immediately while PTT or another
# timing-sensitive radio operation owns the shared mutex.
#
# -U HAVE_HAMLIB -U HAVE_HERMES_SHM: this suite stubs serial/CM108 and installs a
# fake backend, so it must be independent of the hamlib and shared-memory radio
# backends too.  The parent make passes its full CFLAGS (including -DHAVE_HAMLIB
# / -DHAVE_HERMES_SHM when those are detected) down to the sub-make; without
# undefining them here, radio_io.c compiles in its real hamlib/shm calls and the
# link fails on undefined rig_* / shm_* symbols (this test links no -lhamlib and
# no sbitx_io).
test_radio_io_frequency: radio_io/test_radio_io_frequency.c $(UNITY_SRC) \
                         ../radio_io/radio_io.c ../common/hermes_log.c
	$(CC) $(CFLAGS) -U HAVE_HAMLIB -U HAVE_HERMES_SHM -DRADIO_IO_TEST -o $@ $(filter %.c %.a %.o,$^) $(LDFLAGS)

# WebSocket wire format (RFC 6455).  Mercury speaks this protocol with its own
# code since the vendored GPL-2.0-only library was removed, so the handshake
# digest and the frame codec are ours to keep correct.  Pure functions only --
# no sockets, no threads.
test_ws_protocol: gui_interface/test_ws_protocol.c $(UNITY_SRC) \
                  ../gui_interface/websocket/ws_crypto.c \
                  ../gui_interface/websocket/ws_frame.c \
                  ../gui_interface/websocket/ws_json.c \
                  ../gui_interface/websocket/ws_crypto.h \
                  ../gui_interface/websocket/ws_frame.h \
                  ../gui_interface/websocket/ws_json.h
	$(CC) $(CFLAGS) -I../gui_interface/websocket -o $@ \
	    gui_interface/test_ws_protocol.c $(UNITY_SRC) \
	    ../gui_interface/websocket/ws_crypto.c \
	    ../gui_interface/websocket/ws_frame.c \
	    ../gui_interface/websocket/ws_json.c $(LDFLAGS)

# UI status wire format (embedded struct vs remote JSON must not drift)
# ui_status.h is a PREREQUISITE: the device-id widths under test are #defines
# in the header, so without it make will not rebuild and the test silently
# re-runs the previous binary (which is exactly how a mutation check passes
# when it should fail).
test_ui_status: gui_interface/test_ui_status.c $(UNITY_SRC) ../gui_interface/ui_status.c ../gui_interface/ui_status.h
	$(CC) $(CFLAGS) -I../gui_interface -I../datalink_arq -I../modem -I../modem/mfsk -I../common -o $@ \
	    gui_interface/test_ui_status.c $(UNITY_SRC) ../gui_interface/ui_status.c $(LDFLAGS)

# UI device labelling (issue #189: two identical-looking USB codecs).
# ui_communication.h/ui_status.h are PREREQUISITES: the behaviour under test is
# in ui_devices.c but the types are in the headers, and without them make will
# not relink and a mutation check silently re-runs the previous binary.
test_ui_devices: gui_interface/test_ui_devices.c $(UNITY_SRC) \
                 ../gui_interface/ui_devices.c ../gui_interface/ui_communication.h \
                 ../gui_interface/ui_status.h
	$(CC) $(CFLAGS) -I../gui_interface -I../datalink_arq -I../modem -I../modem/mfsk -I../common -o $@ \
	    gui_interface/test_ui_devices.c $(UNITY_SRC) ../gui_interface/ui_devices.c $(LDFLAGS)

# A PTT change from the UI must be applied AND written to the INI (issue #219).
# ui_communication.c and the real INI code are linked; the radio backend, audio,
# modem, ARQ and websocket are stubbed in the test.  ui_communication.h and
# cfg_utils.h are PREREQUISITES so a header change relinks the test.
# -U HAVE_HAMLIB -U HAVE_HERMES_SHM: this suite stubs the radio backend, so it
# must be independent of the hamlib and shared-memory backends too.  The parent
# make passes its full CFLAGS (including -DHAVE_HAMLIB / -DHAVE_HERMES_SHM when
# those are detected) down to the sub-make; without undefining them here,
# ui_communication.c compiles in its real ui_comm_preload_radio_list() -> hamlib
# preload_radio_list() call and the link fails on that undefined symbol (this
# test links no -lhamlib and no rigctl_parse.c).
test_ui_ptt_persist: gui_interface/test_ui_ptt_persist.c $(UNITY_SRC) \
                     ../gui_interface/ui_communication.c ../common/cfg_utils.c \
                     ../common/iniparser/iniparser.c ../common/iniparser/dictionary.c \
                     ../gui_interface/ui_communication.h ../common/cfg_utils.h
	$(CC) $(CFLAGS) -U HAVE_HAMLIB -U HAVE_HERMES_SHM -I../gui_interface -I../datalink_arq -I../modem -I../modem/freedv \
	    -I../common -I../common/iniparser -I../data_interfaces -I../audioio -I../radio_io -o $@ \
	    $(filter %.c %.a %.o,$^) $(LDFLAGS) -lm

clean:
	rm -f $(TEST_BINS) *.d

# Pull in the header dependencies generated by -MMD (see config.mk).  Each test
# binary is compiled+linked in one step, so -MMD writes a .d named after the
# binary; include only those (paths are relative to this directory).
