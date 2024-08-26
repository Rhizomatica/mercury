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


CC=gcc
CFLAGS=-Wall -pedantic -pthread -std=c17 -xc -I../include
ifeq ($(OS),Windows_NT)
	LDFLAGS=-lpthread
else
	LDFLAGS=-lpthread -lrt
endif

.PHONY: all

all: ring_buffer_posix.o shm_posix.o transmitter receiver

ring_buffer_posix.o: ../source/common/ring_buffer_posix.cc ../include/common/ring_buffer_posix.h
	$(CC) $(CFLAGS) -c ../source/common/ring_buffer_posix.cc -o ring_buffer_posix.o

shm_posix.o: ../source/common/shm_posix.cc ../include/common/shm_posix.h
	$(CC) $(CFLAGS) -c ../source/common/shm_posix.cc -o shm_posix.o


transmitter: transmitter.o ring_buffer_posix.o shm_posix.o
	$(CC) -o transmitter transmitter.o ring_buffer_posix.o shm_posix.o $(LDFLAGS)


receiver: receiver.o ring_buffer_posix.o shm_posix.o
	$(CC) -o receiver receiver.o ring_buffer_posix.o shm_posix.o $(LDFLAGS)


.PHONY: clean
clean:
	rm -f *.o transmitter receiver
