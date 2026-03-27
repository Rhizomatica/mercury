include ../config.mk

CFLAGS = $(COMMON_CFLAGS)

# cfg_utils needs headers from several sibling directories
CFG_CFLAGS = $(COMMON_CFLAGS) -I. -I../audioio/ffaudio -I../gui_interface -I../datalink_arq -I../data_interfaces -I../modem/freedv

all: os_interop.o ring_buffer_posix.o shm_posix.o crc6.o hermes_log.o chan.o queue.o \
     cfg_utils.o iniparser/iniparser.o iniparser/dictionary.o

os_interop.o: os_interop.c
	$(CC) $(CFLAGS) -c os_interop.c

ring_buffer_posix.o: ring_buffer_posix.c
	$(CC) $(CFLAGS) -c ring_buffer_posix.c

shm_posix.o: shm_posix.c
	$(CC) $(CFLAGS) -c shm_posix.c

crc6.o: crc6.c
	$(CC) $(CFLAGS) -c crc6.c

hermes_log.o: hermes_log.c hermes_log.h
	$(CC) $(CFLAGS) -c hermes_log.c

chan.o: chan.c chan.h queue.h
	$(CC) $(CFLAGS) -c chan.c

queue.o: queue.c queue.h
	$(CC) $(CFLAGS) -c queue.c

cfg_utils.o: cfg_utils.c cfg_utils.h
	$(CC) $(CFG_CFLAGS) -c cfg_utils.c

iniparser/iniparser.o: iniparser/iniparser.c iniparser/iniparser.h iniparser/dictionary.h
	$(CC) $(CFLAGS) -Iiniparser -c iniparser/iniparser.c -o iniparser/iniparser.o

iniparser/dictionary.o: iniparser/dictionary.c iniparser/dictionary.h
	$(CC) $(CFLAGS) -Iiniparser -c iniparser/dictionary.c -o iniparser/dictionary.o

.PHONY: all clean

clean:
	rm -f *.o iniparser/*.o
