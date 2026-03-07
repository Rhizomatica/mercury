# Mercury Modem examples
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


CXX ?= g++
CFLAGS=-Wall -pthread -std=c++14 -I../include
ifeq ($(OS),Windows_NT)
	LDFLAGS += -static-libgcc -static-libstdc++ -static -l:libwinpthread.a
else
	LDFLAGS=-lpthread -lrt
endif

.PHONY: all

all: ring_buffer_posix.o shm_posix.o os_interop.o transmitter receiver

ring_buffer_posix.o: ../source/common/ring_buffer_posix.cc ../include/common/ring_buffer_posix.h
	$(CXX) $(CFLAGS) -c ../source/common/ring_buffer_posix.cc -o ring_buffer_posix.o

shm_posix.o: ../source/common/shm_posix.cc ../include/common/shm_posix.h
	$(CXX) $(CFLAGS) -c ../source/common/shm_posix.cc -o shm_posix.o

os_interop.o: ../source/common/os_interop.cc ../include/common/os_interop.h
	$(CXX) $(CFLAGS) -c ../source/common/os_interop.cc -o os_interop.o


transmitter: transmitter.o ring_buffer_posix.o shm_posix.o os_interop.o
	$(CXX) -o transmitter transmitter.o ring_buffer_posix.o shm_posix.o os_interop.o $(LDFLAGS)

transmitter.o: transmitter.c
	$(CXX) $(CFLAGS) -c transmitter.c -o transmitter.o

receiver: receiver.o ring_buffer_posix.o shm_posix.o os_interop.o
	$(CXX) -o receiver receiver.o ring_buffer_posix.o shm_posix.o os_interop.o $(LDFLAGS)

receiver.o: receiver.c
	$(CXX) $(CFLAGS) -c receiver.c -o receiver.o


.PHONY: clean
clean:
	rm -f *.o transmitter receiver transmitter.exe receiver.exe
