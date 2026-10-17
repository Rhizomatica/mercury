add_library(freedv STATIC
    freedv_api.c ofdm.c freedv_fsk.c cohpsk.c fsk.c kiss_fft.c freedv_1600.c fdmdv.c
    codec2_fft.c codec2.c pack.c kiss_fftr.c newamp1.c quantise.c lsp.c lpc.c mbest.c
    phase.c sine.c varicode.c freedv_700.c freedv_vhf_framing.c fmfsk.c filter.c
    freedv_data_channel.c ldpc_codes.c mpdecode_core.c interldpc.c golay23.c linreg.c
    nlp.c codebooknewamp1.c codebooknewamp1_energy.c codebooknewamp2.c codebooknewamp2_energy.c
    postfilter.c interp.c codebook.c codebookd.c codebookge.c codebookjmv.c ofdm_mode.c
    HRA_112_112.c HRA_56_56.c HRAb_396_504.c H_256_768_22.c H_256_512_4.c HRAa_1536_512.c
    H_128_256_5.c H_2064_516_sparse.c H_4096_8192_3d.c H_16200_9720.c H_1024_2048_4f.c
    H_212_158.c phi0.c gp_interleaver.c modem_stats.c
)

target_compile_definitions(freedv PRIVATE GIT_HASH=\"unknown\")

target_include_directories(freedv PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")