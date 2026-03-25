# Makefile for WebSocket-based UI communication
#
# Copyright (C) 2026 Rhizomatica
# Author: Pedro Messetti <pedromessetti.rhizomatica@gmail.com>
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

# Test framework
UNITY_SRC = unity/unity.c
UNITY_INC = -Iunity -Ifff

# Project includes
PROJECT_INC = -I../common -I../modem -I../datalink_arq -I../datalink_broadcast \
              -I../data_interfaces -I../gui_interface -I../radio_io -I../audioio/ffaudio

CFLAGS = $(COMMON_CFLAGS) $(UNITY_INC) $(PROJECT_INC)
LDFLAGS = -lpthread -lrt

# Test executables
TEST_BINS = test_ring_buffer

.PHONY: all test clean

all: test

test: $(TEST_BINS)
	@echo "=== Running Unit Tests ==="
	@fail=0; \
	for t in $(TEST_BINS); do \
		echo "--- Running $$t ---"; \
		./$$t || fail=1; \
	done; \
	if [ $$fail -ne 0 ]; then echo "TESTS FAILED"; exit 1; fi
	@echo "=== All Tests Passed ==="

# Common module tests
test_ring_buffer: common/test_ring_buffer.c $(UNITY_SRC) ../common/ring_buffer_posix.c ../common/os_interop.c ../common/shm_posix.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

clean:
	rm -f $(TEST_BINS)
