

## Set this for Windows
#FFAUDIO_LINKFLAGS += -lole32
#OBJS += ffaudio/ffaudio/wasapi.o
#
#FFAUDIO_LINKFLAGS += -ldsound -ldxguid
#OBJS += ffaudio/ffaudio/dsound.o
#
## For Darwin / MacOS X (unsupported)
#FFAUDIO_LINKFLAGS := -framework CoreFoundation -framework CoreAudio
#OBJS=ffaudio/ffaudio/coreaudio.o
#
## For Linux
FFAUDIO_LINKFLAGS += -lpulse
OBJS += ffaudio/ffaudio/pulse.o
#
FFAUDIO_LINKFLAGS += -lasound -lpthread -lrt
OBJS += ffaudio/ffaudio/alsa.o

# On linux we need...
FFAUDIO_LINKFLAGS +=

CC=gcc
CFLAGS=-Wall -pthread -std=gnu99 -I./ffbase/ -I./ffaudio/
# CFLAGS99=-Wall -pedantic -std=c99
LDFLAGS=$(FFAUDIO_LINKFLAGS)

.PHONY: all

all: audioio

ffaudio/ffaudio/dsound.o: ffaudio/ffaudio/dsound.c
		$(CC) $(CFLAGS) -c ffaudio/ffaudio/dsound.c -o ffaudio/ffaudio/dsound.o

ffaudio/ffaudio/coreaudio.o: ffaudio/ffaudio/coreaudio.c
		$(CC) $(CFLAGS) -c ffaudio/ffaudio/coreaudio.c -o ffaudio/ffaudio/coreaudio.o

ffaudio/ffaudio/pulse.o: ffaudio/ffaudio/pulse.c
		$(CC) $(CFLAGS) -c ffaudio/ffaudio/pulse.c -o ffaudio/ffaudio/pulse.o

ffaudio/ffaudio/wasapi.o: ffaudio/ffaudio/wasapi.c
		$(CC) $(CFLAGS) -c ffaudio/ffaudio/wasapi.c -o ffaudio/ffaudio/wasapi.o

ffaudio/ffaudio/alsa.o: ffaudio/ffaudio/alsa.c
	$(CC) $(CFLAGS) -c ffaudio/ffaudio/alsa.c -o ffaudio/ffaudio/alsa.o


audioio.o: audioio.c std.h $(OBJS)
	$(CC) $(CFLAGS) -c audioio.c -o audioio.o

# test code
audioio: audioio.o $(OBJS)
	$(CC) audioio.o $(OBJS) -o audioio $(LDFLAGS)

#test-ring_buffer2: test-ring_buffer2.o ring_buffer2.o
#	$(CC) $(LDFLAGS) -o test-ring_buffer2 test-ring_buffer2.o ring_buffer2.o




.PHONY: clean
clean:
	rm -f *.o ffaudio/ffaudio/*.o audioio
