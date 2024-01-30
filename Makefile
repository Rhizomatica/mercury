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

CPP=g++
LDFLAGS=-lasound
CPPFLAGS=-Ofast -g0 -Wall -Wno-format -std=gnu++14 -I./include
CPP_SOURCES=$(wildcard source/*.cc source/datalink_layer/*.cc source/physical_layer/*.cc)
DOCS=index.html

uname_p := $(shell uname -m)
ifeq (${uname_p},aarch64)
# Raspberry Pi 4 compiler flags:
	CPPFLAGS+=-march=armv8-a+crc
# Raspberry Pi 5 compiler flags:
#	CFLAGS=-march=armv8.2-a+crypto+fp16+rcpc+dotprod
endif


all: mercury doc
	$(CPP) $(CPP_SOURCES) $(LDFLAGS) $(CPPFLAGS) -o $@
	@doxygen ./mercury.doxyfile
	cp ./docs_FSM/*.png html

mercury: $(CPP_SOURCES)
	$(CPP) $(CPP_SOURCES) $(LDFLAGS) $(CPPFLAGS) -o $@


doc: $(CPP_SOURCES)
	@doxygen ./mercury.doxyfile
	cp ./docs_FSM/*.png html

.PHONY: clean install

install: mercury
	install -D mercury /usr/bin/mercury

clean:
	rm -rf mercury
	rm -rf html/
