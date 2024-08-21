

## Windows options...
#FFAUDIO_API := wasapi
#FFAUDIO_LINKFLAGS := -lole32
#
#FFAUDIO_API=dsound
#FFAUDIO_LINKFLAGS := -ldsound -ldxguid

## Darwin / MacOS X
#FFAUDIO_API := coreaudio
#FFAUDIO_LINKFLAGS := -framework CoreFoundation -framework CoreAudio


## Linux options...
#FFAUDIO_API := pulse
#FFAUDIO_LINKFLAGS := -lpulse -lpthread
#
FFAUDIO_API := alsa
FFAUDIO_LINKFLAGS := -lasound

CC=gcc
CFLAGS=-Wall -std=gnu99 -I./ffbase/ -I./ffaudio/  -DFFAUDIO_INTERFACE_DEFAULT_PTR="&ff$(FFAUDIO_API)"
# CFLAGS99=-Wall -pedantic -std=c99
LDFLAGS=$(FFAUDIO_LINKFLAGS)

.PHONY: all

all: audioio

ffaudio/ffaudio/alsa.o:
	$(CC) $(CFLAGS) -c ffaudio/ffaudio/alsa.c -o ffaudio/ffaudio/alsa.o


audioio.o: audioio.c std.h
	$(CC) $(CFLAGS) -c audioio.c -o audioio.o

# test code
audioio: audioio.o ffaudio/ffaudio/alsa.o
	$(CC) audioio.o ffaudio/ffaudio/alsa.o -o audioio $(LDFLAGS)

#test-ring_buffer2: test-ring_buffer2.o ring_buffer2.o
#	$(CC) $(LDFLAGS) -o test-ring_buffer2 test-ring_buffer2.o ring_buffer2.o




.PHONY: clean
clean:
	rm -f *.o ffaudio/ffaudio/alsa.o audioio
