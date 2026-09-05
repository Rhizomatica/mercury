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

include ../config.mk

ifeq ($(OS),Windows_NT)
       OBJS += ffaudio/ffaudio/wasapi.o
       OBJS += ffaudio/ffaudio/dsound.o
else
    UNAME_S := $(shell uname -s)
    ifeq ($(UNAME_S),Linux)
               OBJS += ffaudio/ffaudio/pulse.o
               OBJS += ffaudio/ffaudio/alsa.o
               # ffaudio's OSS backend needs the OSSv4 API (oss_audioinfo,
               # SNDCTL_AUDIOINFO_EX, SNDCTL_DSP_HALT ...).  Stock Linux ships
               # only the OSS3 stub in linux/soundcard.h, which lacks all of
               # them -- oss.c cannot compile there.  The OSSv4 headers arrive
               # with 4Front OSS (oss4-dev on Debian, which *diverts*
               # linux/soundcard.h), so probe for the API instead of guessing
               # from the OS.  No OSSv4 -> no OSS backend, and audioio.c
               # reports the subsystem as unavailable at runtime.
               HAVE_OSS4 := $(shell $(CC) -fsyntax-only oss4-probe.c >/dev/null 2>&1 && echo 1)
        ifeq ($(HAVE_OSS4),1)
               OBJS += ffaudio/ffaudio/oss.o
               OSS_CFLAGS = -DHAVE_OSS4
        endif
    endif
    ifeq ($(UNAME_S),Darwin)
               OBJS=ffaudio/ffaudio/coreaudio.o
    endif
    ifeq ($(UNAME_S),FreeBSD)
               OBJS=ffaudio/ffaudio/oss.o
    endif
endif


CFLAGS = $(COMMON_CFLAGS) -Wno-unused -I./ffbase/ -I./ffaudio/ -I../common $(OSS_CFLAGS)

LDFLAGS=$(FFAUDIO_LINKFLAGS)

.PHONY: all

all: audioio.a

ffaudio/ffaudio/dsound.o: ffaudio/ffaudio/dsound.c
	$(CC) $(CFLAGS) -std=gnu11 -c ffaudio/ffaudio/dsound.c -o ffaudio/ffaudio/dsound.o

ffaudio/ffaudio/coreaudio.o: ffaudio/ffaudio/coreaudio.c
	$(CC) $(CFLAGS) -std=gnu11 -c ffaudio/ffaudio/coreaudio.c -o ffaudio/ffaudio/coreaudio.o

ffaudio/ffaudio/pulse.o: ffaudio/ffaudio/pulse.c
	$(CC) $(CFLAGS) -std=gnu11 -c ffaudio/ffaudio/pulse.c -o ffaudio/ffaudio/pulse.o

ffaudio/ffaudio/wasapi.o: ffaudio/ffaudio/wasapi.c
	$(CC) $(CFLAGS) -std=gnu11 -c ffaudio/ffaudio/wasapi.c -o ffaudio/ffaudio/wasapi.o

ffaudio/ffaudio/alsa.o: ffaudio/ffaudio/alsa.c
	$(CC) $(CFLAGS) -std=gnu11 -c ffaudio/ffaudio/alsa.c -o ffaudio/ffaudio/alsa.o

ffaudio/ffaudio/oss.o: ffaudio/ffaudio/oss.c
	$(CC) $(CFLAGS) -std=gnu11 -c ffaudio/ffaudio/oss.c -o ffaudio/ffaudio/oss.o

audioio.o: audioio.c std.h $(OBJS)
	$(CC) $(CFLAGS) -std=gnu11 -c audioio.c -o audioio.o

resampler.o: resampler.c resampler.h
	$(CC) $(CFLAGS) -std=gnu11 -c resampler.c -o resampler.o

audioio.a: audioio.o resampler.o $(OBJS)
	$(AR) rc audioio.a audioio.o resampler.o $(OBJS)

.PHONY: clean
clean:
	rm -f *.o *.a ffaudio/ffaudio/*.o

# Pull in the header dependencies emitted by -MMD (see config.mk).  Leading
# dash: absent on a first build, which is correct.
-include $(wildcard *.d */*.d */*/*.d)
