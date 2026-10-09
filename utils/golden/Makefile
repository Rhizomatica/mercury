# utils/golden — C-side drivers for the MATLAB golden-model tests
# (docs/MODEM-ANALYSIS.md).  Links against the vendored codec2 library.
include ../../config.mk

FREEDV   = ../../modem/freedv
CFLAGS   = -Wall -Wextra -O2 -std=gnu11 -I$(FREEDV) -I../../common $(EXTRA_CFLAGS)
LDLIBS   = $(FREEDV)/libfreedvdata.a -lm

TOOLS = ldpc_export ldpc_bench llr_bench

all: $(TOOLS)

$(FREEDV)/libfreedvdata.a:
	$(MAKE) -C $(FREEDV) libfreedvdata.a

ldpc_export: ldpc_export.c $(FREEDV)/libfreedvdata.a
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

ldpc_bench: ldpc_bench.c $(FREEDV)/libfreedvdata.a
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

llr_bench: llr_bench.c $(FREEDV)/libfreedvdata.a
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

# Export every code's parity-check matrix into the MATLAB fixtures directory.
FIXTURES = ../../tests/matlab/fixtures/ldpc
export: ldpc_export
	mkdir -p $(FIXTURES) && ./ldpc_export $(FIXTURES)

clean:
	rm -f $(TOOLS) *.o

.PHONY: all export clean
