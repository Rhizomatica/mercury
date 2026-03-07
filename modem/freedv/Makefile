include ../../config.mk

CFLAGS = $(COMMON_CFLAGS) -Wno-strict-overflow -I.

LIBNAME = libfreedvdata.a

OBJS_COMMON = freedv_api.o ofdm.o freedv_fsk.o cohpsk.o fsk.o kiss_fft.o freedv_1600.o fdmdv.o \
              codec2_fft.o codec2.o pack.o kiss_fftr.o newamp1.o quantise.o lsp.o lpc.o mbest.o \
              phase.o sine.o varicode.o freedv_700.o freedv_vhf_framing.o fmfsk.o filter.o \
              freedv_data_channel.o ldpc_codes.o mpdecode_core.o interldpc.o golay23.o linreg.o \
              nlp.o codebooknewamp1.o codebooknewamp1_energy.o codebooknewamp2.o codebooknewamp2_energy.o \
              postfilter.o interp.o codebook.o codebookd.o codebookge.o codebookjmv.o ofdm_mode.o \
              HRA_112_112.o HRA_56_56.o HRAb_396_504.o H_256_768_22.o H_256_512_4.o HRAa_1536_512.o \
              H_128_256_5.o H_2064_516_sparse.o H_4096_8192_3d.o H_16200_9720.o H_1024_2048_4f.o \
              H_212_158.o phi0.o gp_interleaver.o modem_stats.o

all: freedv_data_tx freedv_data_rx freedv_data_raw_tx freedv_data_raw_rx

$(LIBNAME): $(OBJS_COMMON)
	ar rcs $(LIBNAME) $(OBJS_COMMON)


freedv_data_tx: freedv_data_tx.o $(LIBNAME)
	$(CC) $(CFLAGS) -o $@ freedv_data_tx.o $(LIBNAME) -lm

freedv_data_rx: freedv_data_rx.o $(LIBNAME)
	$(CC) $(CFLAGS) -o $@ freedv_data_rx.o $(LIBNAME) -lm

freedv_data_raw_tx: freedv_data_raw_tx.o modem_stats.o $(LIBNAME)
	$(CC) $(CFLAGS) -o $@ freedv_data_raw_tx.o modem_stats.o $(LIBNAME) -lm

freedv_data_raw_rx: freedv_data_raw_rx.o modem_stats.o octave.o $(LIBNAME)
	$(CC) $(CFLAGS) -o $@ freedv_data_raw_rx.o modem_stats.o octave.o $(LIBNAME) -lm


freedv_data_tx.o: freedv_data_tx.c freedv_api.h
	$(CC) $(CFLAGS) -c freedv_data_tx.c

freedv_data_rx.o: freedv_data_rx.c freedv_api.h
	$(CC) $(CFLAGS) -c freedv_data_rx.c

freedv_data_raw_tx.o: freedv_data_raw_tx.c freedv_api.h
	$(CC) $(CFLAGS) -c freedv_data_raw_tx.c

freedv_data_raw_rx.o: freedv_data_raw_rx.c freedv_api.h
	$(CC) $(CFLAGS) -c freedv_data_raw_rx.c

freedv_api.o: freedv_api.c freedv_api.h
	$(CC) $(CFLAGS) -c freedv_api.c

ofdm.o: ofdm.c
	$(CC) $(CFLAGS) -c ofdm.c

freedv_fsk.o: freedv_fsk.c freedv_api.h
	$(CC) $(CFLAGS) -c freedv_fsk.c

freedv_data_channel.o: freedv_data_channel.c freedv_api.h
	$(CC) $(CFLAGS) -c freedv_data_channel.c

cohpsk.o: cohpsk.c
	$(CC) $(CFLAGS) -c cohpsk.c

fsk.o: fsk.c
	$(CC) $(CFLAGS) -c fsk.c

fsk_mod.o: fsk_mod.c
	$(CC) $(CFLAGS) -c fsk_mod.c

fsk_demod.o: fsk_demod.c
	$(CC) $(CFLAGS) -c fsk_demod.c

kiss_fft.o: kiss_fft.c kiss_fft.h
	$(CC) $(CFLAGS) -c kiss_fft.c

freedv_1600.o: freedv_1600.c freedv_api.h
	$(CC) $(CFLAGS) -c freedv_1600.c

fdmdv.o: fdmdv.c freedv_api.h
	$(CC) $(CFLAGS) -c fdmdv.c

codec2_fft.o: codec2_fft.c codec2_fft.h
	$(CC) $(CFLAGS) -c codec2_fft.c

codec2.o: codec2.c codec2.h
	$(CC) $(CFLAGS) -c codec2.c

pack.o: pack.c
	$(CC) $(CFLAGS) -c pack.c

kiss_fftr.o: kiss_fftr.c kiss_fft.h
	$(CC) $(CFLAGS) -c kiss_fftr.c

newamp1.o: newamp1.c newamp1.h
	$(CC) $(CFLAGS) -c newamp1.c

quantise.o: quantise.c quantise.h
	$(CC) $(CFLAGS) -c quantise.c

lsp.o: lsp.c lsp.h
	$(CC) $(CFLAGS) -c lsp.c

lpc.o: lpc.c lpc.h
	$(CC) $(CFLAGS) -c lpc.c

mbest.o: mbest.c mbest.h
	$(CC) $(CFLAGS) -c mbest.c

phase.o: phase.c phase.h
	$(CC) $(CFLAGS) -c phase.c

sine.o: sine.c sine.h
	$(CC) $(CFLAGS) -c sine.c

varicode.o: varicode.c varicode.h
	$(CC) $(CFLAGS) -c varicode.c

freedv_700.o: freedv_700.c freedv_api.h
	$(CC) $(CFLAGS) -c freedv_700.c

freedv_vhf_framing.o: freedv_vhf_framing.c freedv_api.h
	$(CC) $(CFLAGS) -c freedv_vhf_framing.c

fmfsk.o: fmfsk.c fmfsk.h
	$(CC) $(CFLAGS) -c fmfsk.c

filter.o: filter.c filter.h
	$(CC) $(CFLAGS) -c filter.c

ldpc_codes.o: ldpc_codes.c ldpc_codes.h
	$(CC) $(CFLAGS) -c ldpc_codes.c

mpdecode_core.o: mpdecode_core.c mpdecode_core.h
	$(CC) $(CFLAGS) -c mpdecode_core.c

interldpc.o: interldpc.c interldpc.h
	$(CC) $(CFLAGS) -c interldpc.c

golay23.o: golay23.c golay23.h
	$(CC) $(CFLAGS) -c golay23.c

linreg.o: linreg.c linreg.h
	$(CC) $(CFLAGS) -c linreg.c

nlp.o: nlp.c nlp.h
	$(CC) $(CFLAGS) -c nlp.c

codebooknewamp1.o: codebooknewamp1.c
	$(CC) $(CFLAGS) -c codebooknewamp1.c

codebooknewamp1_energy.o: codebooknewamp1_energy.c
	$(CC) $(CFLAGS) -c codebooknewamp1_energy.c

codebooknewamp2.o: codebooknewamp2.c
	$(CC) $(CFLAGS) -c codebooknewamp2.c

codebooknewamp2_energy.o: codebooknewamp2_energy.c
	$(CC) $(CFLAGS) -c codebooknewamp2_energy.c

postfilter.o: postfilter.c
	$(CC) $(CFLAGS) -c postfilter.c

interp.o: interp.c
	$(CC) $(CFLAGS) -c interp.c

codebook.o: codebook.c
	$(CC) $(CFLAGS) -c codebook.c

codebookd.o: codebookd.c
	$(CC) $(CFLAGS) -c codebookd.c

codebookge.o: codebookge.c
	$(CC) $(CFLAGS) -c codebookge.c

codebookjmv.o: codebookjmv.c
	$(CC) $(CFLAGS) -c codebookjmv.c

ofdm_mode.o: ofdm_mode.c
	$(CC) $(CFLAGS) -c ofdm_mode.c

HRA_112_112.o: HRA_112_112.c
	$(CC) $(CFLAGS) -c HRA_112_112.c

HRA_56_56.o: HRA_56_56.c
	$(CC) $(CFLAGS) -c HRA_56_56.c

HRAb_396_504.o: HRAb_396_504.c
	$(CC) $(CFLAGS) -c HRAb_396_504.c

H_256_768_22.o: H_256_768_22.c
	$(CC) $(CFLAGS) -c H_256_768_22.c

H_256_512_4.o: H_256_512_4.c
	$(CC) $(CFLAGS) -c H_256_512_4.c

HRAa_1536_512.o: HRAa_1536_512.c
	$(CC) $(CFLAGS) -c HRAa_1536_512.c

H_128_256_5.o: H_128_256_5.c
	$(CC) $(CFLAGS) -c H_128_256_5.c

H_2064_516_sparse.o: H_2064_516_sparse.c
	$(CC) $(CFLAGS) -c H_2064_516_sparse.c

H_4096_8192_3d.o: H_4096_8192_3d.c
	$(CC) $(CFLAGS) -c H_4096_8192_3d.c

H_16200_9720.o: H_16200_9720.c
	$(CC) $(CFLAGS) -c H_16200_9720.c

H_1024_2048_4f.o: H_1024_2048_4f.c
	$(CC) $(CFLAGS) -c H_1024_2048_4f.c

H_212_158.o: H_212_158.c
	$(CC) $(CFLAGS) -c H_212_158.c

phi0.o: phi0.c phi0.h
	$(CC) $(CFLAGS) -c phi0.c

gp_interleaver.o: gp_interleaver.c gp_interleaver.h
	$(CC) $(CFLAGS) -c gp_interleaver.c

modem_stats.o: modem_stats.c modem_stats.h
	$(CC) $(CFLAGS) -c modem_stats.c

octave.o: octave.c octave.h
	$(CC) $(CFLAGS) -c octave.c

clean:
	rm -f freedv_data_tx freedv_data_rx freedv_data_raw_tx freedv_data_raw_rx
	rm -f *.o *.a

.PHONY: all clean
