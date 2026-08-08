# Compiler and flags
include ../config.mk

CFLAGS = $(COMMON_CFLAGS) -Wextra -I../common -I../modem -I.. -I../modem/freedv
LDFLAGS = -pthread

ifeq ($(OS),Windows_NT)
LDFLAGS += -lws2_32
endif

OUT = broadcast_test broadcast_diag_tx broadcast_diag_rx watterson_test acquire_vs_decode

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

# Clean up build artifacts
clean:
	rm -f $(OUT) $(addsuffix .exe, $(OUT))

# Run the program
run: broadcast_test
	./broadcast_test 127.0.0.1 8300

# Drives the real freedv OFDM modems (not a model) to separate acquisition
# failures from decode failures.  chanutil puts it on the project's own
# Watterson channel so its numbers are comparable with other instruments'.
acquire_vs_decode: acquire_vs_decode.c chanutil.c ../common/watterson.c
	$(CC) $(CFLAGS) -o $@ $^ ../modem/freedv/libfreedvdata.a $(LDFLAGS) -lm

.PHONY: all clean run
