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

AR=ar
CC=gcc
CXX=g++

ifeq ($(OS),Windows_NT)
       OBJS += ffaudio/ffaudio/wasapi.o
       OBJS += ffaudio/ffaudio/dsound.o
else
    UNAME_S := $(shell uname -s)
    ifeq ($(UNAME_S),Linux)
               OBJS += ffaudio/ffaudio/pulse.o
               OBJS += ffaudio/ffaudio/alsa.o
    endif
    ifeq ($(UNAME_S),Darwin)
               OBJS=ffaudio/ffaudio/coreaudio.o
    endif
    ifeq ($(UNAME_S),FreeBSD)
               OBJS=ffaudio/ffaudio/oss.o
    endif
endif


#CFLAGS += -Wall -Og -g -std=gnu99 -I./ffbase/ -I./ffaudio/ -I../../include
CFLAGS = -Wall -O3 -g0 -I./ffbase/ -I./ffaudio/ -I../../include -pthread

LDFLAGS=$(FFAUDIO_LINKFLAGS)

.PHONY: all

all: audioio.a

ffaudio/ffaudio/dsound.o: ffaudio/ffaudio/dsound.c
	$(CC) $(CFLAGS) -std=c17 -c ffaudio/ffaudio/dsound.c -o ffaudio/ffaudio/dsound.o

ffaudio/ffaudio/coreaudio.o: ffaudio/ffaudio/coreaudio.c
	$(CC) $(CFLAGS) -std=c17 -c ffaudio/ffaudio/coreaudio.c -o ffaudio/ffaudio/coreaudio.o

ffaudio/ffaudio/pulse.o: ffaudio/ffaudio/pulse.c
	$(CC) $(CFLAGS) -std=c17 -c ffaudio/ffaudio/pulse.c -o ffaudio/ffaudio/pulse.o

ffaudio/ffaudio/wasapi.o: ffaudio/ffaudio/wasapi.c
	$(CC) $(CFLAGS) -std=c17 -c ffaudio/ffaudio/wasapi.c -o ffaudio/ffaudio/wasapi.o

ffaudio/ffaudio/alsa.o: ffaudio/ffaudio/alsa.c
	$(CC) $(CFLAGS) -std=c17 -c ffaudio/ffaudio/alsa.c -o ffaudio/ffaudio/alsa.o

audioio.o: audioio.c std.h $(OBJS)
	$(CXX) $(CFLAGS) -std=c++14 -xc++ -c audioio.c -o audioio.o

audioio.a: audioio.o $(OBJS)
	$(AR) rc audioio.a audioio.o $(OBJS)

.PHONY: clean
clean:
	rm -f *.o *.a ffaudio/ffaudio/*.o
