# Compiler and flags
include ../config.mk

CFLAGS = $(COMMON_CFLAGS) -Wextra -I../common -I../modem -I.. -I../modem/freedv
LDFLAGS = -pthread

ifeq ($(OS),Windows_NT)
LDFLAGS += -lws2_32
endif

OUT = broadcast_test broadcast_diag_tx broadcast_diag_rx watterson_test shortframe_sweep acquire_vs_decode hail_suffix_sweep

# Default target
all: $(OUT)

broadcast_test: broadcast_test.c ../datalink_broadcast/kiss.c
	$(CC) $(CFLAGS) -I../datalink_broadcast -o $@ $^ $(LDFLAGS)

broadcast_diag_tx: broadcast_diag_tx.c ../datalink_broadcast/kiss.c ../modem/framer.c
	$(CC) $(CFLAGS) -I../datalink_broadcast -o $@ $^ $(LDFLAGS)

broadcast_diag_rx: broadcast_diag_rx.c ../datalink_broadcast/kiss.c ../modem/framer.c
	$(CC) $(CFLAGS) -I../datalink_broadcast -o $@ $^ $(LDFLAGS)

watterson_test: watterson_test.c ../common/watterson.c
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
hail_suffix_sweep: hail_suffix_sweep.c ../modem/mfsk.c ../modem/mfsk_sync.c ../modem/mfsk_ofdm.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) -lm

acquire_vs_decode: acquire_vs_decode.c
	$(CC) $(CFLAGS) -o $@ $^ ../modem/freedv/libfreedvdata.a $(LDFLAGS) -lm

.PHONY: all clean run
