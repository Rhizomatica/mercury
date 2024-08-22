# Audio subsystem
#
# Copyright (C) 2024 Rhizomatica
# Author: Rafael Diniz <rafael@riseup.net>
#
# This is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 3, or (at your option)
# any later version.
#
# This software is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this software; see the file COPYING.  If not, write to
# the Free Software Foundation, Inc., 51 Franklin Street,
# Boston, MA 02110-1301, USA.
#

ifeq ($(OS),Windows_NT)
	FFAUDIO_LINKFLAGS += -lole32
	OBJS += ffaudio/ffaudio/wasapi.o
	FFAUDIO_LINKFLAGS += -ldsound -ldxguid
	OBJS += ffaudio/ffaudio/dsound.o
else
    UNAME_S := $(shell uname -s)
    ifeq ($(UNAME_S),Linux)
		FFAUDIO_LINKFLAGS += -lpulse
		OBJS += ffaudio/ffaudio/pulse.o
		FFAUDIO_LINKFLAGS += -lasound -lpthread -lrt
		OBJS += ffaudio/ffaudio/alsa.o
		CFLAGS += -pthread
    endif
    ifeq ($(UNAME_S),Darwin)
		FFAUDIO_LINKFLAGS := -framework CoreFoundation -framework CoreAudio
		OBJS=ffaudio/ffaudio/coreaudio.o
    endif
    ifeq ($(UNAME_S),FreeBSD)
		FFAUDIO_LINKFLAGS := -lm
		OBJS=ffaudio/ffaudio/oss.o
    endif
endif

AR=ar
CC=gcc
#CFLAGS += -Wall -O0 -g -std=gnu99 -I./ffbase/ -I./ffaudio/ -I../../include
CFLAGS += -Wall -O2 -std=c17 -I./ffbase/ -I./ffaudio/ -I../../include
LDFLAGS=$(FFAUDIO_LINKFLAGS)

.PHONY: all

all: audioio.a

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

../common/ring_buffer_posix.o: ../common/ring_buffer_posix.cc
	$(CC) $(CFLAGS) -xc -c ../common/ring_buffer_posix.cc -o ../common/ring_buffer_posix.o

../common/shm_posix.o: ../common/shm_posix.cc
	$(CC) $(CFLAGS) -xc -c ../common/shm_posix.cc -o ../common/shm_posix.o

audioio.o: audioio.c std.h $(OBJS)
	$(CC) $(CFLAGS) -c audioio.c -o audioio.o

audioio.a: audioio.o ../common/ring_buffer_posix.o ../common/shm_posix.o $(OBJS)
	$(AR) rc audioio.a audioio.o ../common/ring_buffer_posix.o ../common/shm_posix.o $(OBJS)

.PHONY: clean
clean:
	rm -f *.o *.a ffaudio/ffaudio/*.o
