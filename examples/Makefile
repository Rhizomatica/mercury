CC=gcc
CFLAGS=-Wall -pedantic -pthread -std=c17 -xc -I../include
LDFLAGS=-lpthread -lrt

.PHONY: all

all: ring_buffer_posix.o shm_posix.o transmitter receiver

ring_buffer_posix.o: ../source/common/ring_buffer_posix.cc ../include/common/ring_buffer_posix.h
	$(CC) $(CFLAGS) -c ../source/common/ring_buffer_posix.cc -o ring_buffer_posix.o

shm_posix.o: ../source/common/shm_posix.cc ../include/common/shm_posix.h
	$(CC) $(CFLAGS) -c ../source/common/shm_posix.cc -o shm_posix.o


transmitter: transmitter.o ring_buffer_posix.o shm_posix.o
	$(CC) $(LDFLAGS) -o transmitter transmitter.o ring_buffer_posix.o shm_posix.o


receiver: receiver.o ring_buffer_posix.o shm_posix.o
	$(CC) $(LDFLAGS) -o receiver receiver.o ring_buffer_posix.o shm_posix.o


.PHONY: clean
clean:
	rm -f *.o transmitter receiver
