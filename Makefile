# Mercury Modem
#
#    This program is free software: you can redistribute it and/or modify
#    it under the terms of the GNU Affero General Public License as published by
#    the Free Software Foundation, either version 3 of the License, or
#    (at your option) any later version.
#
#    This program is distributed in the hope that it will be useful,
#    but WITHOUT ANY WARRANTY; without even the implied warranty of
#    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#    GNU Affero General Public License for more details.
#
#    You should have received a copy of the GNU Affero General Public License
#    along with this program.  If not, see <https://www.gnu.org/licenses/>.
#
#

ifeq ($(OS),Windows_NT)
	FFAUDIO_LINKFLAGS += -lole32
	FFAUDIO_LINKFLAGS += -ldsound -ldxguid
	FFAUDIO_LINKFLAGS += -lws2_32
	FFAUDIO_LINKFLAGS += -static-libgcc -static-libstdc++ -static -l:libwinpthread.a
else
	UNAME_S := $(shell uname -s)
    ifeq ($(UNAME_S),Linux)
		FFAUDIO_LINKFLAGS += -lpulse
		FFAUDIO_LINKFLAGS += -lasound -lpthread -lrt
    endif
    ifeq ($(UNAME_S),Darwin)
		FFAUDIO_LINKFLAGS := -framework CoreFoundation -framework CoreAudio
    endif
    ifeq ($(UNAME_S),FreeBSD)
		FFAUDIO_LINKFLAGS := -lm
    endif
endif

#WATCOM_CFLAGS="-bm"
#GCC_CFLAGS="-lpthread -lrt"

CPP=g++
LDFLAGS=$(FFAUDIO_LINKFLAGS)
CPPFLAGS=-Ofast -g0 -Wall -Wextra -Wno-format -std=c++14 -I./include -I./source/audioio/ffaudio -pthread
#CPPFLAGS=-Ofast -g0 -Wall -fstack-protector -D_FORTIFY_SOURCE=2 -Wno-format -std=gnu++14 -I./include
CPP_SOURCES=$(wildcard source/*.cc source/datalink_layer/*.cc source/physical_layer/*.cc source/common/*.cc)
OBJECT_FILES=$(patsubst %.cc,%.o,$(CPP_SOURCES))

DOCS=index.html

uname_m := $(shell uname -m)
ifeq (${uname_m},aarch64)
# Raspberry Pi 4 compiler flags:
	CPPFLAGS+=-march=armv8-a+crc
# Raspberry Pi 5 compiler flags (comment above and uncomment below):
#	CPPFLAGS+=-march=armv8.2-a+crypto+fp16+rcpc+dotprod
endif

.PHONY: clean install examples audioio

all: mercury examples

examples:
	$(MAKE) -C examples

source/audioio/audioio.a: source/audioio/audioio.c
	$(MAKE) -C source/audioio

mercury: $(OBJECT_FILES) source/audioio/audioio.a
	$(CPP) -o $@ $(OBJECT_FILES) source/audioio/audioio.a $(LDFLAGS)

%.o : %.cc %.h
	$(CPP) -c $(CPPFLAGS) $< -o $@

doc: $(CPP_SOURCES)
	@doxygen ./mercury.doxyfile
	cp ./docs_FSM/*.png html



install: mercury
	install -D mercury /usr/bin/mercury

clean:
	rm -rf mercury $(OBJECT_FILES)
	rm -rf html/
	$(MAKE) -C examples clean
	$(MAKE) -C source/audioio clean
