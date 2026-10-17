# utils/golden — C measurement tools for the LDPC path (docs/LDPC-DECODER.md).
#
#   ldpc_sweep   FER against Es/N0 per decoder variant, the LLR-scale cost at
#                the real modulation, the min-sum scale sweep and the cliff
#                fixtures: the source of every curve under
#                docs/ldpc-figures/data (make sweep).
#   ldpc_bench   encode or decode frames from files with an iteration cap, an
#                algorithm and a deadline; times the decoder in process
#                (decoder_timing.csv comes from its summary line).
#   llr_bench    the soft demappers on a file of symbols; dumps a constellation.
#   ldpc_export  every code's parity-check matrix as explicit rows.
#
# The tools link against the vendored codec2 library.  ldpc_sweep also
# compiles the project's reproducible random source, which is not in it.
#
#   make                          build the tools
#   make export                   parity-check matrices into fixtures/ldpc
#   make -j4 sweep                regenerate the record's data files
#   make SANITIZE_ASAN_UBSAN=1    instrumented build; rebuild the library the
#                                 same way first (make -C ../../modem/freedv
#                                 clean, then the same flag)
include ../../config.mk

FREEDV   = ../../modem/freedv
COMMON   = ../../common
CFLAGS   = -Wall -Wextra -O2 -std=gnu11 -I$(FREEDV) -I$(COMMON) $(EXTRA_CFLAGS) $(SAN_FLAGS)
LDLIBS   = $(FREEDV)/libfreedvdata.a -lm $(SAN_LDFLAGS)

TOOLS = ldpc_export ldpc_bench llr_bench ldpc_sweep

all: $(TOOLS)

$(FREEDV)/libfreedvdata.a:
	$(MAKE) -C $(FREEDV) libfreedvdata.a

ldpc_export: ldpc_export.c $(FREEDV)/libfreedvdata.a
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

ldpc_bench: ldpc_bench.c $(FREEDV)/libfreedvdata.a
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

llr_bench: llr_bench.c $(FREEDV)/libfreedvdata.a
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

ldpc_sweep: ldpc_sweep.c $(COMMON)/mercury_prng.c $(FREEDV)/libfreedvdata.a
	$(CC) $(CFLAGS) -o $@ ldpc_sweep.c $(COMMON)/mercury_prng.c $(LDLIBS)

# Every code's parity-check matrix, exactly as the decoder uses it.
FIXTURES = fixtures/ldpc
export: ldpc_export
	mkdir -p $(FIXTURES) && ./ldpc_export $(FIXTURES)

# ---- the record's data files ----------------------------------------------
# make -j<n> sweep runs the codes in parallel.  Each data file is a real
# target, so a second "make sweep" does nothing until ldpc_sweep changes;
# "make -B sweep" regenerates everything.  SEED replays the same frames.  The
# crossing summaries ldpc_sweep prints go to results/<name>.txt and are shown
# at the end.
SEED ?= 1
DATA  = ../../docs/ldpc-figures/data

# the 11 codes with a FER curve: the seven data-mode codes, then voice and 2020
FER_CODES = HRA_56_56 H_128_256_5 H_256_512_4 H_256_768_22 H_1024_2048_4f \
            H_4096_8192_3d H_16200_9720 HRA_112_112 HRAb_396_504 \
            HRAa_1536_512 H_2064_516_sparse
# the three code/modulation pairs of the LLR-scale figure (defaults in ldpc_sweep)
LLR_CODES = H_256_768_22 H_4096_8192_3d H_16200_9720
# the four codes of the ARQ ladder: min-sum scale sweep and cliff fixtures
LADDER    = H_256_768_22 H_1024_2048_4f H_4096_8192_3d H_16200_9720
# cliff fixture names: <code>_esno<Es/N0 dB>, the points ldpc_bench times
CLIFF     = H_256_768_22_esno-3.0 H_1024_2048_4f_esno-1.5 \
            H_4096_8192_3d_esno-1.4 H_16200_9720_esno-0.9

FER_CSV   = $(FER_CODES:%=$(DATA)/ldpc_fer_%.csv)
LLR_CSV   = $(LLR_CODES:%=$(DATA)/llr_scale_%.csv)
NMS_LINES = $(LADDER:%=results/nms_scale_%.txt)
CLIFF_F32 = $(CLIFF:%=fixtures/llr/%.f32)

sweep: $(FER_CSV) $(LLR_CSV) $(DATA)/nms_scale_c.txt $(DATA)/above_cliff_H_4096_8192_3d.csv $(CLIFF_F32)
	-cat $(FER_CODES:%=results/ldpc_fer_%.txt) $(LLR_CODES:%=results/llr_scale_%.txt) $(DATA)/nms_scale_c.txt

$(DATA)/ldpc_fer_%.csv: ldpc_sweep | results
	./ldpc_sweep --fer --code $* --seed $(SEED) --out $@ > results/ldpc_fer_$*.txt

$(DATA)/llr_scale_%.csv: ldpc_sweep | results
	./ldpc_sweep --llr-scale --code $* --seed $(SEED) --out $@ > results/llr_scale_$*.txt

results/nms_scale_%.txt: ldpc_sweep | results
	./ldpc_sweep --nms-scale --code $* --seed $(SEED) --out results/nms_scale_$*.csv > $@

$(DATA)/nms_scale_c.txt: $(NMS_LINES)
	cat $^ > $@

# the record's above-the-cliff comparison on DATAC1's code: one point at
# Eb/N0 1.25 dB, 2000 frames, the four context decoders at cap 50
$(DATA)/above_cliff_H_4096_8192_3d.csv: ldpc_sweep | results
	./ldpc_sweep --fer --code H_4096_8192_3d --ebno 1.25:0.5:1.25 --frames 2000 --min-errors 2000 \
	    --seed $(SEED) --variants C_sp50,C_spt50,C_nms50,C_nms16_50 --out $@ > results/above_cliff_H_4096_8192_3d.txt

# the stem is <code>_esno<dB>: split it at "_esno"
fixtures/llr/%.f32: ldpc_sweep
	mkdir -p fixtures/llr
	./ldpc_sweep --fixture --code $(firstword $(subst _esno, ,$*)) \
	    --esno $(lastword $(subst _esno, ,$*)) --frames 100 --seed $(SEED) --out fixtures/llr/$*

results:
	mkdir -p results

clean:
	rm -f $(TOOLS) *.o
	rm -rf results fixtures

.PHONY: all export sweep clean
