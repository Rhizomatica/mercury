# utils/loopsim — build the noisebridge channel helper.
CC ?= gcc
CFLAGS ?= -Wall -O2 -std=gnu11

all: noisebridge

noisebridge: noisebridge.c
	$(CC) $(CFLAGS) -o $@ noisebridge.c -lm

clean:
	rm -f noisebridge

.PHONY: all clean
