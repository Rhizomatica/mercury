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
