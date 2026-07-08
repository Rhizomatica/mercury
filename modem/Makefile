include ../config.mk

CFLAGS = $(COMMON_CFLAGS) -Wno-strict-overflow -fPIC -I. -I../common -I../modem -I../modem/freedv -I../datalink_arq -I../data_interfaces

.PHONY: all freedv 

all: freedv modem.o framer.o channel_busy.o

freedv:
	$(MAKE) -C freedv

modem.o: modem.c modem.h
	$(CC) $(CFLAGS) -c modem.c

channel_busy.o: channel_busy.c channel_busy.h
	$(CC) $(CFLAGS) -c channel_busy.c

framer.o: framer.c framer.h
	$(CC) $(CFLAGS) -c framer.c

clean:
	$(MAKE) -C freedv clean
	rm -f *.o
