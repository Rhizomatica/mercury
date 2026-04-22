# Compiler and flags
include ../config.mk

CFLAGS = $(COMMON_CFLAGS) -Wextra -I../common -I../modem
LDFLAGS = -pthread

ifeq ($(OS),Windows_NT)
LDFLAGS += -lws2_32
endif

OUT = broadcast_test broadcast_diag_tx broadcast_diag_rx

# Default target
all: $(OUT)

broadcast_test: broadcast_test.c ../datalink_broadcast/kiss.c
	$(CC) $(CFLAGS) -I../datalink_broadcast -o $@ $^ $(LDFLAGS)

broadcast_diag_tx: broadcast_diag_tx.c ../datalink_broadcast/kiss.c ../modem/framer.c
	$(CC) $(CFLAGS) -I../datalink_broadcast -o $@ $^ $(LDFLAGS)

broadcast_diag_rx: broadcast_diag_rx.c ../datalink_broadcast/kiss.c ../modem/framer.c
	$(CC) $(CFLAGS) -I../datalink_broadcast -o $@ $^ $(LDFLAGS)

# Clean up build artifacts
clean:
	rm -f $(OUT) $(addsuffix .exe, $(OUT))

# Run the program
run: broadcast_test
	./broadcast_test 127.0.0.1 8300

.PHONY: all clean run
