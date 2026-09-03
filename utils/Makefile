# Compiler and flags
include ../config.mk

CFLAGS = $(COMMON_CFLAGS) -Wextra -I../common -I../modem -I.. -I../modem/freedv
LDFLAGS = -pthread

ifeq ($(OS),Windows_NT)
LDFLAGS += -lws2_32
endif

OUT = broadcast_test broadcast_diag_tx broadcast_diag_rx watterson_test shortframe_sweep acquire_vs_decode hail_suffix_sweep ackpat_sweep mfsk_puncture_sweep mfsk_floor_sweep

# CPU-cost instruments.  Linux/macOS only: they time with
# clock_gettime(CLOCK_PROCESS_CPUTIME_ID), which mingw-w64 does not provide
# (undefined reference to clock_gettime at link).  They are development tools
# for profiling the RX path, so there is nothing to gain from porting them to
# the Windows build -- just exclude them and keep the cross-compile green.
ifneq ($(OS),Windows_NT)
OUT += resampler_bench rxcost_bench

# bcast_file_tool talks to Mercury's broadcast port with BSD sockets
# (<arpa/inet.h>, <netinet/tcp.h>, unistd read/write), which mingw-w64 does not
# have -- Windows wants winsock2.  It is a development/diagnostic tool for
# driving a broadcast file transfer without the UI, so there is nothing to gain
# from porting it; the UI on Windows uses the same code in-process over CGo.
OUT += bcast_file_tool
endif

# Default target
all: $(OUT)

broadcast_test: broadcast_test.c ../datalink_broadcast/kiss.c
	$(CC) $(CFLAGS) -I../datalink_broadcast -o $@ $^ $(LDFLAGS)

broadcast_diag_tx: broadcast_diag_tx.c ../datalink_broadcast/kiss.c ../modem/framer.c
	$(CC) $(CFLAGS) -I../datalink_broadcast -o $@ $^ $(LDFLAGS)

broadcast_diag_rx: broadcast_diag_rx.c ../datalink_broadcast/kiss.c ../modem/framer.c
	$(CC) $(CFLAGS) -I../datalink_broadcast -o $@ $^ $(LDFLAGS)

watterson_test: watterson_test.c ../common/watterson.c ../common/mercury_prng.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) -lm

# Needs the freedv LDPC decoder and its vendored parity matrices.
shortframe_sweep: shortframe_sweep.c ../modem/mfsk.c ../modem/freedv/mpdecode_core.c \
                  ../modem/freedv/ldpc_codes.c ../modem/freedv/phi0.c
	$(CC) $(CFLAGS) -o $@ $^ ../modem/freedv/libfreedvdata.a $(LDFLAGS) -lm

# Clean up build artifacts
clean:
	rm -f $(OUT) $(addsuffix .exe, $(OUT))

# Run the program
run: broadcast_test
	./broadcast_test 127.0.0.1 8300

# Links the whole freedv library: drives the real OFDM modems, not a model.
# Is the MFSK floor set by acquisition or by the code?  Sweeps noise over the
# whole burst and over the payload alone; the gap is the code's slack.
mfsk_floor_sweep: mfsk_floor_sweep.c chanutil.c ../common/watterson.c ../common/mercury_prng.c \
                  ../modem/modem_mfsk.c ../modem/mfsk.c ../modem/mfsk_sync.c \
                  ../modem/mfsk_ofdm.c ../modem/mfsk_ldpc.c ../modem/mfsk_ldpc_8_16.c
	$(CC) $(CFLAGS) -o $@ $^ ../modem/freedv/libfreedvdata.a $(LDFLAGS) -lm

# Coding cost of shortening the MFSK floor burst by puncturing parity.
mfsk_puncture_sweep: mfsk_puncture_sweep.c ../modem/mfsk_ldpc.c ../modem/mfsk_ldpc_8_16.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) -lm

# What the pattern ACK's second repetition buys: detection vs SNR and
# false-accept on noise, for one repetition and two, at every threshold.
ackpat_sweep: ackpat_sweep.c ../modem/modem_mfsk.c ../modem/mfsk.c \
              ../modem/mfsk_sync.c ../modem/mfsk_ofdm.c ../modem/mfsk_ldpc.c ../modem/mfsk_ldpc_8_16.c
	$(CC) $(CFLAGS) -o $@ $^ ../modem/freedv/libfreedvdata.a $(LDFLAGS) -lm

hail_suffix_sweep: hail_suffix_sweep.c chanutil.c ../common/watterson.c ../common/mercury_prng.c \
                   ../modem/mfsk.c ../modem/mfsk_sync.c ../modem/mfsk_ofdm.c
	$(CC) $(CFLAGS) -o $@ $^ ../modem/freedv/libfreedvdata.a $(LDFLAGS) -lm

# Drives the real freedv OFDM modems (not a model) to separate acquisition
# failures from decode failures.  chanutil puts it on the project's own
# Watterson channel so its numbers are comparable with other instruments'.
acquire_vs_decode: acquire_vs_decode.c chanutil.c ../common/watterson.c ../common/mercury_prng.c
	$(CC) $(CFLAGS) -o $@ $^ ../modem/freedv/libfreedvdata.a $(LDFLAGS) -lm

# Perf instruments for issue #162 (idle CPU on a 48 kHz Pi 4).
resampler_bench: resampler_bench.c ../audioio/resampler.c
	$(CC) $(CFLAGS) -I../audioio -o $@ $^ $(LDFLAGS) -lm

rxcost_bench: rxcost_bench.c
	$(CC) $(CFLAGS) -o $@ $^ ../modem/freedv/libfreedvdata.a $(LDFLAGS) -lm

.PHONY: all clean run

# Broadcast a file to Mercury, or receive one, over the broadcast TCP/KISS port.
BCAST_RQ_SRC = $(wildcard ../datalink_broadcast/raptorq/lib/*.c) \
               $(wildcard ../datalink_broadcast/raptorq/deps/obl/*.c)
bcast_file_tool: bcast_file_tool.c ../datalink_broadcast/bcast_file.c $(BCAST_RQ_SRC)
	$(CC) $(CFLAGS) -I../datalink_broadcast -I../datalink_broadcast/raptorq/include \
	      -I../datalink_broadcast/raptorq/deps -o $@ $^ $(LDFLAGS) -lm
