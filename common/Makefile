include ../config.mk

CFLAGS = -MMD -MP $(COMMON_CFLAGS)

# cfg_utils needs headers from several sibling directories
CFG_CFLAGS = $(COMMON_CFLAGS) -I. -I../audioio/ffaudio -I../gui_interface -I../datalink_arq -I../data_interfaces -I../modem/freedv

# -S (HERMES SHM radio control) is gated by HAVE_HERMES_SHM, exported by the
# root Makefile (1 on Linux, 0 for the Windows cross-build) — mirror main.o.
ifeq ($(HAVE_HERMES_SHM),1)
CLI_SHM_CFLAGS = -DHAVE_HERMES_SHM
endif

# Shared engine + CLI parser need the sibling module headers.
ENGINE_CFLAGS = $(CFG_CFLAGS) -I.. -I../modem -I../modem/freedv -I../datalink_broadcast -I../data_interfaces -I../datalink_arq -I../audioio -I../radio_io

all: os_interop.o ring_buffer_posix.o shm_posix.o crc6.o hermes_log.o chan.o queue.o \
     virtual_clock.o cfg_utils.o iniparser/iniparser.o iniparser/dictionary.o watterson.o mercury_engine.o \
     mercury_cli.o

os_interop.o: os_interop.c
	$(CC) $(CFLAGS) -c os_interop.c

ring_buffer_posix.o: ring_buffer_posix.c
	$(CC) $(CFLAGS) -c ring_buffer_posix.c

shm_posix.o: shm_posix.c
	$(CC) $(CFLAGS) -c shm_posix.c

crc6.o: crc6.c
	$(CC) $(CFLAGS) -c crc6.c

virtual_clock.o: virtual_clock.c virtual_clock.h
	$(CC) $(CFLAGS) -c virtual_clock.c

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

watterson.o: watterson.c watterson.h
	$(CC) $(CFLAGS) -I.. -c watterson.c

mercury_engine.o: mercury_engine.c mercury_engine.h
	$(CC) $(ENGINE_CFLAGS) -c mercury_engine.c

mercury_cli.o: mercury_cli.c mercury_cli.h
	$(CC) $(ENGINE_CFLAGS) $(CLI_SHM_CFLAGS) -c mercury_cli.c

.PHONY: all clean

clean:
	rm -f *.o iniparser/*.o

# Auto header dependencies (see top-level Makefile note): stay at END.
-include $(wildcard *.d */*.d)
