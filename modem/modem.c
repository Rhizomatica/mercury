/* HERMES Modem
 *
 * Copyright (C) 2025 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3, or (at your option)
 * any later version.
 *
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 */


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <stdint.h>
#include <time.h>

#include "modem.h"
#include "ring_buffer_posix.h"
#include "framer.h"
#include "arq.h"
#include "../datalink_arq/arq_modem.h"
#include "tcp_interfaces.h"
#include "freedv_api.h"
#include "fsk.h"
#include "ldpc_codes.h"
#include "ofdm_internal.h"
#include "hermes_log.h"

#include "defines_modem.h"

/* Here we read and write from the input and output buffers which are connected */
/* to the radio dsp paths. */

/* The buffers can be connected to an external software (eg. sbitx_controller)  */
/* or to the audioio subsystem which connects to a sound card. */

extern bool shutdown_; // global shutdown flag

extern cbuf_handle_t capture_buffer;
extern cbuf_handle_t playback_buffer;

extern char *freedv_mode_names[];

cbuf_handle_t data_tx_buffer_arq;
cbuf_handle_t data_tx_buffer_arq_control;
cbuf_handle_t data_rx_buffer_arq;

cbuf_handle_t data_tx_buffer_broadcast;
cbuf_handle_t data_rx_buffer_broadcast;

pthread_t tx_thread_tid, rx_thread_tid;
static pthread_mutex_t modem_freedv_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t modem_freedv_epoch = 1;
static uint64_t modem_last_switch_ms = 0;

/* --- Spectrum data for UI waterfall display --- */
#include "freedv/modem_stats.h"
static float g_rx_spectrum_dB[MODEM_STATS_NSPEC];
static pthread_mutex_t g_spectrum_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_spectrum_sample_rate = 8000;
static bool g_spectrum_valid = false;
static struct MODEM_STATS g_spectrum_stats;
static bool g_spectrum_stats_inited = false;
#define ARQ_ACTION_WAIT_MS 100
#define RX_TX_DRAIN_SAMPLES 160
#define RX_DECODE_CHUNK_SAMPLES 160
#define RX_IDLE_SLEEP_US 5000

typedef struct {
    struct freedv *datac1;
    struct freedv *datac3;
    struct freedv *datac4;
    struct freedv *datac13;
    size_t payload_datac1;
    size_t payload_datac3;
    size_t payload_datac4;
    size_t payload_datac13;
} modem_mode_pool_t;

static modem_mode_pool_t modem_mode_pool = {0};

static uint64_t monotonic_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((uint64_t)ts.tv_sec * 1000ULL) + ((uint64_t)ts.tv_nsec / 1000000ULL);
}

static bool arq_mode_policy_ready_snapshot(bool have_snapshot, const arq_runtime_snapshot_t *snapshot)
{
    return have_snapshot && snapshot && snapshot->initialized;
}

static bool is_supported_split_mode(int mode)
{
    return mode == FREEDV_MODE_DATAC1 ||
           mode == FREEDV_MODE_DATAC3 ||
           mode == FREEDV_MODE_DATAC4 ||
           mode == FREEDV_MODE_DATAC13;
}

static bool is_payload_split_mode(int mode)
{
    return mode == FREEDV_MODE_DATAC1 ||
           mode == FREEDV_MODE_DATAC3 ||
           mode == FREEDV_MODE_DATAC4;
}

static const char *mode_name_from_enum(int mode)
{
    switch (mode)
    {
    case FREEDV_MODE_DATAC1: return "DATAC1";
    case FREEDV_MODE_DATAC3: return "DATAC3";
    case FREEDV_MODE_DATAC0: return "DATAC0";
    case FREEDV_MODE_DATAC4: return "DATAC4";
    case FREEDV_MODE_DATAC13: return "DATAC13";
    case FREEDV_MODE_DATAC14: return "DATAC14";
    case FREEDV_MODE_FSK_LDPC: return "FSK_LDPC";
    default: return "UNKNOWN";
    }
}

static struct freedv *open_freedv_mode_locked(int mode)
{
    // Was:
    // char codename[80] = "H_256_512_4";
    // struct freedv_advanced adv = {0, 2, 100, 8000, 1000, 200, codename};
    char codename[80] = "H_256_768_22";
    struct freedv_advanced adv = {0, 4, 50, 8000, 750, 250, codename};

    if (mode == FREEDV_MODE_FSK_LDPC)
        return freedv_open_advanced(mode, &adv);
    return freedv_open(mode);
}

static void clear_mode_pool_locked(void)
{
    if (modem_mode_pool.datac1) freedv_close(modem_mode_pool.datac1);
    if (modem_mode_pool.datac3) freedv_close(modem_mode_pool.datac3);
    if (modem_mode_pool.datac4) freedv_close(modem_mode_pool.datac4);
    if (modem_mode_pool.datac13) freedv_close(modem_mode_pool.datac13);
    memset(&modem_mode_pool, 0, sizeof(modem_mode_pool));
}

static int pool_open_mode_locked(struct freedv **slot, size_t *payload_slot, int mode, int frames_per_burst, int freedv_verbosity)
{
    struct freedv *f = open_freedv_mode_locked(mode);
    if (!f)
        return -1;
    freedv_set_frames_per_burst(f, frames_per_burst);
    freedv_set_verbose(f, freedv_verbosity);
    *slot = f;
    *payload_slot = (freedv_get_bits_per_modem_frame(f) / 8) - 2;
    return 0;
}

static int init_mode_pool_locked(int frames_per_burst, int freedv_verbosity)
{
    clear_mode_pool_locked();
    if (pool_open_mode_locked(&modem_mode_pool.datac13, &modem_mode_pool.payload_datac13, FREEDV_MODE_DATAC13, frames_per_burst, freedv_verbosity) < 0)
        goto fail;
    if (pool_open_mode_locked(&modem_mode_pool.datac4, &modem_mode_pool.payload_datac4, FREEDV_MODE_DATAC4, frames_per_burst, freedv_verbosity) < 0)
        goto fail;
    if (pool_open_mode_locked(&modem_mode_pool.datac3, &modem_mode_pool.payload_datac3, FREEDV_MODE_DATAC3, frames_per_burst, freedv_verbosity) < 0)
        goto fail;
    if (pool_open_mode_locked(&modem_mode_pool.datac1, &modem_mode_pool.payload_datac1, FREEDV_MODE_DATAC1, frames_per_burst, freedv_verbosity) < 0)
        goto fail;
    return 0;
fail:
    clear_mode_pool_locked();
    return -1;
}

static struct freedv *pooled_freedv_for_mode_locked(int mode, size_t *payload_bytes)
{
    switch (mode)
    {
    case FREEDV_MODE_DATAC1:
        if (payload_bytes) *payload_bytes = modem_mode_pool.payload_datac1;
        return modem_mode_pool.datac1;
    case FREEDV_MODE_DATAC3:
        if (payload_bytes) *payload_bytes = modem_mode_pool.payload_datac3;
        return modem_mode_pool.datac3;
    case FREEDV_MODE_DATAC4:
        if (payload_bytes) *payload_bytes = modem_mode_pool.payload_datac4;
        return modem_mode_pool.datac4;
    case FREEDV_MODE_DATAC13:
        if (payload_bytes) *payload_bytes = modem_mode_pool.payload_datac13;
        return modem_mode_pool.datac13;
    default:
        if (payload_bytes) *payload_bytes = 0;
        return NULL;
    }
}

static bool is_pooled_freedv_locked(struct freedv *f)
{
    return f &&
           (f == modem_mode_pool.datac1 ||
            f == modem_mode_pool.datac3 ||
            f == modem_mode_pool.datac4 ||
            f == modem_mode_pool.datac13);
}

static uint32_t compute_bitrate_bps_locked(struct freedv *freedv)
{
    uint32_t bits_per_modem_frame = (uint32_t)freedv_get_bits_per_modem_frame(freedv);
    uint32_t tx_modem_samples = (uint32_t)freedv_get_n_tx_modem_samples(freedv);
    uint32_t modem_sample_rate = (uint32_t)freedv_get_modem_sample_rate(freedv);

    if (tx_modem_samples == 0)
        return 0;

    return (uint32_t)(((uint64_t)bits_per_modem_frame * modem_sample_rate + (tx_modem_samples / 2)) / tx_modem_samples);
}

static uint32_t bitrate_level_from_payload_mode(int mode)
{
    switch (mode)
    {
    case FREEDV_MODE_DATAC1:
        return 1;
    case FREEDV_MODE_DATAC3:
        return 3;
    case FREEDV_MODE_DATAC4:
        return 4;
    default:
        return 4;
    }
}

static int select_payload_rx_mode(const arq_runtime_snapshot_t *snapshot, bool ready)
{
    int mode = FREEDV_MODE_DATAC4;

    if (!ready || !snapshot)
        return mode;

    if (is_payload_split_mode(snapshot->preferred_rx_mode))
        mode = snapshot->preferred_rx_mode;
    else if (is_payload_split_mode(snapshot->peer_tx_mode))
        mode = snapshot->peer_tx_mode;  /* peer's TX mode = what our decoder must match */

    return mode;
}

static int maybe_switch_modem_mode(generic_modem_t *g_modem,
                                   int target_mode,
                                   int arq_trx,
                                   bool force_now)
{
    if (!is_supported_split_mode(target_mode))
        return 0;
    if (arq_trx == TX && !force_now)
        return 0;

    pthread_mutex_lock(&modem_freedv_lock);
    int current_mode = g_modem->mode;
    if (current_mode == target_mode)
    {
        pthread_mutex_unlock(&modem_freedv_lock);
        return 0;
    }
    uint64_t now_ms = monotonic_ms();
    if (!force_now &&
        modem_last_switch_ms != 0 &&
        (now_ms - modem_last_switch_ms) < 250)
    {
        pthread_mutex_unlock(&modem_freedv_lock);
        return 0;
    }
    size_t payload_bytes_per_modem_frame = 0;
    struct freedv *new_freedv = pooled_freedv_for_mode_locked(target_mode, &payload_bytes_per_modem_frame);
    if (!new_freedv)
    {
        pthread_mutex_unlock(&modem_freedv_lock);
        return -1;
    }
    g_modem->freedv = new_freedv;
    g_modem->mode = target_mode;
    g_modem->payload_bytes_per_modem_frame = payload_bytes_per_modem_frame;
    modem_freedv_epoch++;
    modem_last_switch_ms = now_ms;
    pthread_mutex_unlock(&modem_freedv_lock);
    arq_set_active_modem_mode(target_mode, payload_bytes_per_modem_frame);

    HLOGD("modem", "Switched modem mode to %d (%s), payload=%zu",
          target_mode, mode_name_from_enum(target_mode), payload_bytes_per_modem_frame);
    return 1;
}

int init_modem(generic_modem_t *g_modem, int mode, int frames_per_burst, int test_mode, int freedv_verbosity)
{
// connect to shared memory buffers (skip if audioio already set them up)
    bool shm_connected = false;
    if (capture_buffer == NULL)
    {
try_shm_connect1:
        capture_buffer = circular_buf_connect_shm(SIGNAL_BUFFER_SIZE, SIGNAL_INPUT);
        if (capture_buffer == NULL)
        {
            printf("Shared memory not created. Waiting for the radio daemon\n");
            sleep(1);
            goto try_shm_connect1;
        }
        shm_connected = true;
    }

    if (playback_buffer == NULL)
    {
try_shm_connect2:
        playback_buffer = circular_buf_connect_shm(SIGNAL_BUFFER_SIZE, SIGNAL_OUTPUT);
        if (playback_buffer == NULL)
        {
            printf("Shared memory not created. Waiting for the radio daemon...\n");
            sleep(1);
            goto try_shm_connect2;
        }
    }

    if (shm_connected)
        printf("Connected to Shared Memory Radio I/O tx/rx buffers.\n");

    // buffers for the ARQ datalink
    uint8_t *buffer_tx = (uint8_t *) malloc(DATA_TX_BUFFER_SIZE);
    uint8_t *buffer_tx_control = (uint8_t *) malloc(DATA_TX_BUFFER_SIZE);
    uint8_t *buffer_rx = (uint8_t *) malloc(DATA_RX_BUFFER_SIZE);
    data_tx_buffer_arq = circular_buf_init(buffer_tx, DATA_TX_BUFFER_SIZE);
    data_tx_buffer_arq_control = circular_buf_init(buffer_tx_control, DATA_TX_BUFFER_SIZE);
    data_rx_buffer_arq = circular_buf_init(buffer_rx, DATA_RX_BUFFER_SIZE);

    // buffers for the broadcast datalink
    buffer_tx = (uint8_t *) malloc(DATA_TX_BUFFER_SIZE);
    buffer_rx = (uint8_t *) malloc(DATA_RX_BUFFER_SIZE);
    data_tx_buffer_broadcast = circular_buf_init(buffer_tx, DATA_TX_BUFFER_SIZE);
    data_rx_buffer_broadcast = circular_buf_init(buffer_rx, DATA_RX_BUFFER_SIZE);
    
    printf("Created data buffers for ARQ and BROADCAST datalink, tx/rx paths.\n");

    pthread_mutex_lock(&modem_freedv_lock);
    if (init_mode_pool_locked(frames_per_burst, freedv_verbosity) < 0)
    {
        pthread_mutex_unlock(&modem_freedv_lock);
        fprintf(stderr, "Failed to initialize persistent FreeDV pool\n");
        return -1;
    }

    size_t payload_bytes_per_modem_frame = 0;
    g_modem->freedv = pooled_freedv_for_mode_locked(mode, &payload_bytes_per_modem_frame);
    if (!g_modem->freedv)
    {
        g_modem->freedv = open_freedv_mode_locked(mode);
        if (!g_modem->freedv)
        {
            pthread_mutex_unlock(&modem_freedv_lock);
            fprintf(stderr, "Failed to open FreeDV mode %d\n", mode);
            return -1;
        }
        freedv_set_frames_per_burst(g_modem->freedv, frames_per_burst);
        freedv_set_verbose(g_modem->freedv, freedv_verbosity);
        payload_bytes_per_modem_frame = (freedv_get_bits_per_modem_frame(g_modem->freedv) / 8) - 2;
    }
    pthread_mutex_unlock(&modem_freedv_lock);

    g_modem->mode = mode;
    g_modem->payload_bytes_per_modem_frame = payload_bytes_per_modem_frame;
    
    int modem_sample_rate = freedv_get_modem_sample_rate(g_modem->freedv);
    printf("Initialized persistent FreeDV mode pool (DATAC13/DATAC4/DATAC3/DATAC1), frames per burst: %d\n", frames_per_burst);
    printf("Active FreeDV mode at startup: %d (%s), verbosity: %d\n", mode, mode_name_from_enum(mode), freedv_verbosity);
    printf("Modem expects sample rate: %d Hz\n", modem_sample_rate);
    printf("Modem payload bytes per frame: %zu\n", payload_bytes_per_modem_frame);
    printf("Split control/data mode switching: ENABLED\n");
    
    if (modem_sample_rate != 8000)
    {
        printf("WARNING: Modem sample rate is %d Hz, but audio I/O is configured for 8kHz!\n", modem_sample_rate);
        printf("         You need to adjust the resampling in audioio.c or use a different mode.\n");
    }

    // test if testing is enable
    if(test_mode == 1) // tx
    {
        run_tests_tx(g_modem);
    }
    if(test_mode == 2) // rx
    {
        run_tests_rx(g_modem);
    }

    // Create TX and RX threads
    pthread_create(&tx_thread_tid, NULL, tx_thread, (void *)g_modem);
    pthread_create(&rx_thread_tid, NULL, rx_thread, (void *)g_modem);

    return 0;
}

static void drain_capture_buffer_fast(size_t samples)
{
    static int32_t *discard = NULL;
    static size_t discard_cap = 0;

    if (samples == 0)
        return;

    if (discard_cap < samples)
    {
        int32_t *new_discard = (int32_t *)realloc(discard, samples * sizeof(int32_t));
        if (!new_discard)
            return;
        discard = new_discard;
        discard_cap = samples;
    }

    read_buffer(capture_buffer, (uint8_t *)discard, samples * sizeof(int32_t));
}

int run_tests_tx(generic_modem_t *g_modem)
{
    size_t bytes_per_modem_frame = freedv_get_bits_per_modem_frame(g_modem->freedv) / 8;
    size_t payload_size = bytes_per_modem_frame - 2;  /* 2 bytes reserved for CRC by send_modulated_data */
    uint8_t *buffer = (uint8_t *)malloc(payload_size);

    if (!buffer)
    {
        printf("ERROR: Failed to allocate TX test buffer\n");
        return -1;
    }

    printf("TX test: bytes_per_modem_frame=%zu, payload_size=%zu\n",
           bytes_per_modem_frame, payload_size);

    int counter = 0;

    while(1)
    {
        for (size_t i = 0; i < payload_size; i++)
        {
            buffer[i] = 0;
        }
        buffer[counter % payload_size] = 1;
        counter++;

        send_modulated_data(g_modem, buffer, 1);

        // lets not forget to discard the input buffer... as we are just transmitting
        size_t rx_discard = size_buffer(capture_buffer);
        if (rx_discard > 0)
        {
            clear_buffer(capture_buffer);
        }
    }

    free(buffer);
    return 0;
}


int run_tests_rx(generic_modem_t *g_modem)
{
    size_t bytes_per_modem_frame = freedv_get_bits_per_modem_frame(g_modem->freedv) / 8;
    size_t payload_size = bytes_per_modem_frame - 2;  /* RX returns frame with CRC, payload is 2 bytes less */
    uint8_t *buffer = (uint8_t *)malloc(bytes_per_modem_frame);

    if (!buffer)
    {
        printf("ERROR: Failed to allocate RX test buffer\n");
        return -1;
    }

    printf("RX test: bytes_per_modem_frame=%zu, payload_size=%zu\n",
           bytes_per_modem_frame, payload_size);

    size_t bytes_out = 0;
    int counter = 0;

    while(!shutdown_)
    {
        receive_modulated_data(g_modem, buffer, &bytes_out);
        if (bytes_out == 0)
        {
            usleep(RX_IDLE_SLEEP_US);
            continue;
        }

        counter++;
        /* bytes_out includes CRC, actual payload is bytes_out - 2 */
        size_t payload_len = (bytes_out >= 2) ? bytes_out - 2 : bytes_out;

        printf("Frame %d (%zu payload bytes):\n", counter, payload_len);
        for (size_t j = 0; j < payload_len; j++)
        {
            printf("%02x ", buffer[j]);
            if ((j + 1) % 16 == 0) printf("\n");
        }
        printf("\n");
        usleep(RX_IDLE_SLEEP_US);
    }

    free(buffer);
    return 0;
}

int shutdown_modem(generic_modem_t *g_modem)
{
    // Wait for threads to finish
    pthread_join(tx_thread_tid, NULL);
    pthread_join(rx_thread_tid, NULL);
    
    if (capture_buffer) {
	    circular_buf_disconnect_shm(capture_buffer, SIGNAL_BUFFER_SIZE);
	    circular_buf_free_shm(capture_buffer);
    }
    if (playback_buffer) {
	    circular_buf_disconnect_shm(playback_buffer, SIGNAL_BUFFER_SIZE);
	    circular_buf_free_shm(playback_buffer);
    }
    circular_buf_free_shm(capture_buffer);
    circular_buf_free_shm(playback_buffer);

    free(data_tx_buffer_arq->buffer);
    free(data_tx_buffer_arq_control->buffer);
    free(data_rx_buffer_arq->buffer);
    free(data_tx_buffer_broadcast->buffer);
    free(data_rx_buffer_broadcast->buffer);
    circular_buf_free(data_tx_buffer_arq);
    circular_buf_free(data_tx_buffer_arq_control);
    circular_buf_free(data_rx_buffer_arq);
    circular_buf_free(data_tx_buffer_broadcast);
    circular_buf_free(data_rx_buffer_broadcast);
    
    pthread_mutex_lock(&modem_freedv_lock);
    if (g_modem->freedv && !is_pooled_freedv_locked(g_modem->freedv))
        freedv_close(g_modem->freedv);
    g_modem->freedv = NULL;
    clear_mode_pool_locked();
    pthread_mutex_unlock(&modem_freedv_lock);

    return 0;
}

int send_modulated_data(generic_modem_t *g_modem, uint8_t *bytes_in, int frames_per_burst)
{
    pthread_mutex_lock(&modem_freedv_lock);
    struct freedv *freedv = g_modem->freedv;
    size_t bytes_per_modem_frame = freedv_get_bits_per_modem_frame(freedv) / 8;
    size_t payload_bytes = bytes_per_modem_frame - 2;  /* 2 bytes reserved for CRC16 */
    size_t n_mod_out = freedv_get_n_tx_modem_samples(freedv);
    uint8_t frame_with_crc[bytes_per_modem_frame];

    /* Inter-burst silence */
    int inter_burst_delay_ms = 200;
    int samples_silence = FREEDV_FS_8000 * inter_burst_delay_ms / 1000;
    if (freedv_get_mode(freedv) == FREEDV_MODE_FSK_LDPC)
    {
        int fsk_settle_samples = freedv_get_n_nom_modem_samples(freedv);
        if (fsk_settle_samples > samples_silence)
            samples_silence = fsk_settle_samples;
    }

    /* 100ms head silence before preamble — gives the radio RX→TX relay and
     * the peer's audio path time to settle before the preamble arrives. */
    int samples_head = FREEDV_FS_8000 * 100 / 1000;

    /* Calculate max buffer size needed:
     * head silence + preamble + (frames * n_mod_out) + postamble + tail silence */
    int max_preamble = freedv_get_n_tx_modem_samples(freedv) * 2;  /* conservative estimate */
    int max_postamble = max_preamble;
    size_t max_samples = (size_t)samples_head + max_preamble + (frames_per_burst * n_mod_out) + max_postamble + samples_silence;

    /* Allocate temporary buffer for all modulated audio */
    int32_t *tx_buffer = (int32_t *)malloc(max_samples * sizeof(int32_t));
    int16_t *mod_out_short = (int16_t *)malloc(n_mod_out * sizeof(int16_t));

    if (!tx_buffer || !mod_out_short)
    {
        printf("ERROR: Failed to allocate TX buffer\n");
        if (tx_buffer) free(tx_buffer);
        if (mod_out_short) free(mod_out_short);
        pthread_mutex_unlock(&modem_freedv_lock);
        return -1;
    }

    int total_samples = 0;


    /* === STEP 1: Generate all modulated audio into temp buffer === */

    /* Head silence: allow relay/audio path to settle before preamble */
    for (int i = 0; i < samples_head; i++)
        tx_buffer[total_samples++] = 0;

    /* Generate preamble */
    int n_preamble = freedv_rawdatapreambletx(freedv, mod_out_short);
    for (int i = 0; i < n_preamble; i++)
    {
        tx_buffer[total_samples++] = (int32_t)mod_out_short[i] << 16;
    }

    /* Generate data frame(s) */
    for (int i = 0; i < frames_per_burst; i++)
    {
        /* Copy payload and add CRC16 in last 2 bytes */
        memcpy(frame_with_crc, &bytes_in[payload_bytes * i], payload_bytes);
        uint16_t crc16 = freedv_gen_crc16(frame_with_crc, payload_bytes);
        frame_with_crc[bytes_per_modem_frame - 2] = crc16 >> 8;
        frame_with_crc[bytes_per_modem_frame - 1] = crc16 & 0xff;

        freedv_rawdatatx(freedv, mod_out_short, frame_with_crc);
        for (size_t j = 0; j < n_mod_out; j++)
        {
            tx_buffer[total_samples++] = (int32_t)mod_out_short[j] << 16;
        }
    }

    /* Generate postamble */
    int n_postamble = freedv_rawdatapostambletx(freedv, mod_out_short);
    for (int i = 0; i < n_postamble; i++)
    {
        tx_buffer[total_samples++] = (int32_t)mod_out_short[i] << 16;
    }

    /* Add silence at end */
    for (int i = 0; i < samples_silence; i++)
    {
        tx_buffer[total_samples++] = 0;
    }
    pthread_mutex_unlock(&modem_freedv_lock);


    /* === STEP 2: Key transmitter and send pre-generated audio === */

    ptt_on();
    arq_modem_ptt_on(freedv_get_mode(g_modem->freedv),
                     freedv_get_bits_per_modem_frame(g_modem->freedv) / 8);
    
    /* Wait for radio relay to switch (10ms for your radio) */
    usleep(10000);

    /* Write entire pre-generated buffer to playback */
    write_buffer(playback_buffer, (uint8_t *)tx_buffer, total_samples * sizeof(int32_t));

    /* Wait for all samples to be played out */
    usleep(total_samples * 1000000 / FREEDV_FS_8000);

    /* Give some tail time before turning off PTT */
    usleep(TAIL_TIME_US);

    ptt_off();
    arq_modem_ptt_off();

    free(tx_buffer);
    free(mod_out_short);

    return 0;
}

int receive_modulated_data(generic_modem_t *g_modem, uint8_t *bytes_out, size_t *nbytes_out)
{
    struct freedv *freedv = NULL;
    uint64_t epoch = 0;
    size_t nin = 0;
    int input_size = 0;
    bool idle_spin_sleep = false;

    static int frames_received = 0;
    static int16_t *demod_in = NULL;
    static int32_t *buffer_in = NULL;
    static int buffer_size = 0;

    if (!g_modem || !bytes_out || !nbytes_out)
        return -1;

    pthread_mutex_lock(&modem_freedv_lock);
    freedv = g_modem->freedv;
    epoch = modem_freedv_epoch;
    if (!freedv)
    {
        pthread_mutex_unlock(&modem_freedv_lock);
        *nbytes_out = 0;
        usleep(RX_IDLE_SLEEP_US);
        return 0;
    }
    input_size = freedv_get_n_max_modem_samples(freedv);
    nin = freedv_nin(freedv);
    pthread_mutex_unlock(&modem_freedv_lock);
    
    // Allocate buffers on first call or if size changed
    if (buffer_size < input_size)
    {
        int16_t *new_demod_in = (int16_t *)realloc(demod_in, input_size * sizeof(int16_t));
        int32_t *new_buffer_in = NULL;
        if (!new_demod_in)
        {
            *nbytes_out = 0;
            return -1;
        }
        demod_in = new_demod_in;
        new_buffer_in = (int32_t *)realloc(buffer_in, input_size * sizeof(int32_t));
        if (!new_buffer_in)
        {
            *nbytes_out = 0;
            return -1;
        }
        buffer_in = new_buffer_in;
        buffer_size = input_size;
    }
    if (!demod_in || !buffer_in)
    {
        *nbytes_out = 0;
        return -1;
    }

    // Safety check: nin should not be larger than buffer
    if (nin > (size_t)input_size)
    {
        fprintf(stderr, "RX error: nin=%zu exceeds input_size=%d\n", nin, input_size);
        *nbytes_out = 0;
        return -1;
    }

    // Read samples only when nin > 0
    // When nin == 0, FreeDV has buffered enough samples and will process internally
    if (nin > 0)
    {
        read_buffer(capture_buffer, (uint8_t *) buffer_in, sizeof(int32_t) * nin);

        // converting from s32le to s16le
        for (size_t i = 0; i < nin; i++)
        {
            demod_in[i] = (int16_t)(buffer_in[i] >> 16);
        }
    }

    pthread_mutex_lock(&modem_freedv_lock);
    if (g_modem->freedv != freedv || modem_freedv_epoch != epoch)
    {
        pthread_mutex_unlock(&modem_freedv_lock);
        *nbytes_out = 0;
        return 0;
    }

    // ALWAYS call freedv_rawdatarx - even when nin==0, it processes internal buffers
    *nbytes_out = freedv_rawdatarx(freedv, bytes_out, demod_in);
    if (nin == 0 && *nbytes_out == 0)
        idle_spin_sleep = true;

    int sync = 0;
    float snr_est = 0.0;
    freedv_get_modem_stats(freedv, &sync, &snr_est);
    if (!sync)
        *nbytes_out = 0;

    if (*nbytes_out > 0)
    {
        frames_received++;
        printf(">>> DECODED FRAME %d: %zu bytes, SNR: %.2f dB\n",
               frames_received, *nbytes_out, snr_est);
    }

    pthread_mutex_unlock(&modem_freedv_lock);
    if (idle_spin_sleep)
        usleep(RX_IDLE_SLEEP_US);
    return 0;
}

typedef struct {
    struct freedv *freedv;
    int mode;
    int16_t *demod_in;
    int demod_count;
    int demod_cap;
    uint8_t *bytes_out;
    size_t bytes_cap;
    int last_sync;      /* previous sync state, for change detection */
} rx_decoder_state_t;

typedef struct {
    int sync;
    int rx_status;
    float snr_est;
    bool snr_valid;
    bool frame_decoded;
} rx_metrics_accum_t;

static void rx_metrics_update(rx_metrics_accum_t *metrics, int sync, float snr, int rx_status, bool frame_decoded)
{
    if (!metrics)
        return;

    if (sync)
        metrics->sync = 1;
    metrics->rx_status |= rx_status;
    if (!metrics->snr_valid || snr > metrics->snr_est)
    {
        metrics->snr_est = snr;
        metrics->snr_valid = true;
    }
    if (frame_decoded)
        metrics->frame_decoded = true;
}

static int rx_decoder_bind_mode(rx_decoder_state_t *state, int mode)
{
    struct freedv *freedv = NULL;
    int max_samples = 0;
    size_t bytes_cap = 0;
    bool mode_changed = false;

    if (!state || !is_supported_split_mode(mode))
        return -1;

    pthread_mutex_lock(&modem_freedv_lock);
    freedv = pooled_freedv_for_mode_locked(mode, NULL);
    if (freedv)
    {
        max_samples = freedv_get_n_max_modem_samples(freedv);
        bytes_cap = (size_t)(freedv_get_bits_per_modem_frame(freedv) / 8);
        mode_changed = state->freedv != freedv || state->mode != mode;
        if (mode_changed)
        {
            /* Do NOT call freedv_set_sync(UNSYNC) here.  The new mode's
             * decoder is already in search mode from its last use (or
             * init).  UNSYNC would zero its rxbuf, starving the preamble
             * correlation normalizer (same as the was_tx regression).
             * Stale rxbuf content is harmless: it won't correlate with
             * the new mode's preamble pattern, and the non-zero energy
             * keeps the normalizer well-conditioned. */
            state->demod_count = 0;
        }
    }
    pthread_mutex_unlock(&modem_freedv_lock);

    if (!freedv || max_samples <= 0 || bytes_cap == 0)
        return -1;

    if (state->demod_cap < max_samples)
    {
        int16_t *new_demod = (int16_t *)realloc(state->demod_in, (size_t)max_samples * sizeof(int16_t));
        if (!new_demod)
            return -1;
        state->demod_in = new_demod;
        state->demod_cap = max_samples;
    }

    if (state->bytes_cap < bytes_cap)
    {
        uint8_t *new_bytes = (uint8_t *)realloc(state->bytes_out, bytes_cap);
        if (!new_bytes)
            return -1;
        state->bytes_out = new_bytes;
        state->bytes_cap = bytes_cap;
    }

    state->freedv = freedv;
    state->mode = mode;
    return 0;
}

static void rx_decoder_dispose(rx_decoder_state_t *state)
{
    if (!state)
        return;
    free(state->demod_in);
    free(state->bytes_out);
    memset(state, 0, sizeof(*state));
}

static int rx_decoder_target_chunk_samples(const rx_decoder_state_t *state)
{
    int nin = 0;

    if (!state || !state->freedv)
        return RX_DECODE_CHUNK_SAMPLES;

    pthread_mutex_lock(&modem_freedv_lock);
    nin = freedv_nin(state->freedv);
    pthread_mutex_unlock(&modem_freedv_lock);

    if (nin < RX_DECODE_CHUNK_SAMPLES)
        nin = RX_DECODE_CHUNK_SAMPLES;
    return nin;
}

static void process_received_frame(const uint8_t *data,
                                   size_t nbytes_out,
                                   size_t frame_bytes,
                                   bool arq_policy_ready,
                                   int payload_mode,
                                   uint32_t bitrate_bps,
                                   float snr_est)
{
    size_t payload_nbytes;
    int frame_type;

    if (!data || nbytes_out < 2)
        return;

    payload_nbytes = nbytes_out - 2;
    if (payload_nbytes == 0)
        return;

    tnc_send_sn(snr_est);
    tnc_send_bitrate(bitrate_level_from_payload_mode(payload_mode), bitrate_bps);

    frame_type = parse_frame_header((uint8_t *)data, payload_nbytes);

    switch (frame_type)
    {
    case PACKET_TYPE_ARQ_CALL:
        if (arq_policy_ready)
            arq_handle_incoming_connect_frame((uint8_t *)data, payload_nbytes);
        break;
    case PACKET_TYPE_ARQ_CONTROL:
    case PACKET_TYPE_ARQ_DATA:
        if (arq_policy_ready)
            arq_handle_incoming_frame((uint8_t *)data, payload_nbytes, snr_est);
        break;
    case PACKET_TYPE_BROADCAST_CONTROL:
    case PACKET_TYPE_BROADCAST_DATA:
        write_buffer(data_rx_buffer_broadcast, (uint8_t *)data, payload_nbytes);
        break;
    default:
        HLOGW("modem-rx", "Unknown frame type received");
        break;
    }

    HLOGD("modem-rx", "Frame rx bytes=%zu type=%d frame_bytes=%zu",
          payload_nbytes, frame_type, frame_bytes);
}

static void rx_decoder_consume_chunk(rx_decoder_state_t *state,
                                     const int16_t *samples,
                                     int sample_count,
                                     bool arq_policy_ready,
                                     int payload_mode,
                                     uint32_t bitrate_bps,
                                     rx_metrics_accum_t *metrics)
{
    int guard = 0;

    if (!state || !state->freedv || !state->demod_in || !state->bytes_out ||
        !samples || sample_count <= 0)
        return;

    if (sample_count > state->demod_cap)
    {
        samples += sample_count - state->demod_cap;
        sample_count = state->demod_cap;
    }

    if ((size_t)state->demod_count + (size_t)sample_count > (size_t)state->demod_cap)
    {
        size_t overflow = ((size_t)state->demod_count + (size_t)sample_count) - (size_t)state->demod_cap;
        if (overflow >= (size_t)state->demod_count)
        {
            state->demod_count = 0;
        }
        else
        {
            memmove(state->demod_in,
                    state->demod_in + overflow,
                    ((size_t)state->demod_count - overflow) * sizeof(int16_t));
            state->demod_count -= (int)overflow;
        }
    }

    memcpy(state->demod_in + state->demod_count, samples, (size_t)sample_count * sizeof(int16_t));
    state->demod_count += sample_count;

    while (guard++ < 32)
    {
        int nin = 0;
        int sync = 0;
        int rx_status = 0;
        float snr_est = 0.0f;
        size_t nbytes_out = 0;

        pthread_mutex_lock(&modem_freedv_lock);
        if (!state->freedv)
        {
            pthread_mutex_unlock(&modem_freedv_lock);
            break;
        }

        nin = freedv_nin(state->freedv);
        if (nin < 0 || nin > state->demod_count)
        {
            rx_status = freedv_get_rx_status(state->freedv);
            freedv_get_modem_stats(state->freedv, &sync, &snr_est);
            pthread_mutex_unlock(&modem_freedv_lock);
            rx_metrics_update(metrics, sync, snr_est, rx_status, false);
            break;
        }

        nbytes_out = freedv_rawdatarx(state->freedv, state->bytes_out, state->demod_in);
        if (nin > 0)
        {
            state->demod_count -= nin;
            if (state->demod_count > 0)
            {
                memmove(state->demod_in,
                        state->demod_in + nin,
                        (size_t)state->demod_count * sizeof(int16_t));
            }
        }

        rx_status = freedv_get_rx_status(state->freedv);
        freedv_get_modem_stats(state->freedv, &sync, &snr_est);
        pthread_mutex_unlock(&modem_freedv_lock);

        rx_metrics_update(metrics, sync, snr_est, rx_status, nbytes_out > 0);

        if (nbytes_out > 0)
        {
            HLOGD("modem-rx", "Decoded frame mode=%d (%s) bytes=%zu snr=%.2f",
                  state->mode,
                  mode_name_from_enum(state->mode),
                  nbytes_out,
                  snr_est);
            process_received_frame(state->bytes_out,
                                   nbytes_out,
                                   state->bytes_cap,
                                   arq_policy_ready,
                                   payload_mode,
                                   bitrate_bps,
                                   snr_est);
        }

        if (nin == 0 && nbytes_out == 0)
            break;
    }

    if (guard > 32)
    {
        HLOGW("modem-rx",
              "rx_decoder_consume_chunk guard limit reached (mode=%d, demod_count=%d)",
              state->mode,
              state->demod_count);
    }
}


// Threads


// Function to handle TX logic for the broadcast tx path
void *tx_thread(void *g_modem)
{
    generic_modem_t *modem = (generic_modem_t *)g_modem;
    uint8_t *data = NULL;
    size_t data_size = 0;
    int startup_mode = -1;

    while (!shutdown_)
    {
        arq_runtime_snapshot_t arq_snapshot;
        memset(&arq_snapshot, 0, sizeof(arq_snapshot));
        bool have_arq_snapshot = arq_get_runtime_snapshot(&arq_snapshot);
        bool arq_policy_ready = arq_mode_policy_ready_snapshot(have_arq_snapshot, &arq_snapshot);

        if (startup_mode < 0)
        {
            pthread_mutex_lock(&modem_freedv_lock);
            startup_mode = modem->mode;
            pthread_mutex_unlock(&modem_freedv_lock);
        }

        size_t pending_arq_data = size_buffer(data_tx_buffer_arq);
        size_t pending_arq_control = size_buffer(data_tx_buffer_arq_control);
        size_t pending_broadcast = size_buffer(data_tx_buffer_broadcast);
        int pending_arq_app = have_arq_snapshot ? arq_snapshot.tx_backlog_bytes : 0;
        bool arq_tx_queued =
            (pending_arq_app > 0) ||
            (pending_arq_data > 0) ||
            (pending_arq_control > 0);
        bool local_tx_queued =
            arq_tx_queued ||
            (pending_broadcast > 0);
        if (arq_policy_ready && arq_snapshot.trx != TX && arq_tx_queued)
        {
            int desired_mode = arq_snapshot.preferred_tx_mode;
            if (desired_mode >= 0)
                maybe_switch_modem_mode(modem, desired_mode, arq_snapshot.trx, false);
        }
        else if (arq_snapshot.trx != TX &&
                 !arq_tx_queued &&
                 pending_broadcast > 0 &&
                 startup_mode >= 0)
        {
            // Keep pure broadcast TX on the startup payload mode/frame size.
            maybe_switch_modem_mode(modem, startup_mode, arq_snapshot.trx, false);
        }

        size_t payload_bytes_per_modem_frame = 0;
        int frames_per_burst = 1;
        int tx_frames_per_burst = 1;
        pthread_mutex_lock(&modem_freedv_lock);
        payload_bytes_per_modem_frame = modem->payload_bytes_per_modem_frame;
        if (modem->freedv)
            frames_per_burst = freedv_get_frames_per_burst(modem->freedv);
        pthread_mutex_unlock(&modem_freedv_lock);

        if (payload_bytes_per_modem_frame != 14)
            tx_frames_per_burst = frames_per_burst;

        size_t required = payload_bytes_per_modem_frame * (size_t)tx_frames_per_burst;
        if (required == 0)
        {
            usleep(100000);
            continue;
        }

        if (data_size != required)
        {
            uint8_t *new_data = (uint8_t *)realloc(data, required);
            if (!new_data)
            {
                fprintf(stderr, "Failed to allocate memory for TX data.\n");
                usleep(100000);
                continue;
            }
            data = new_data;
            data_size = required;
        }

        bool sent_from_action = false;
        bool waited_for_action = false;
        arq_action_t action = {0};
        bool have_action = arq_try_dequeue_action(&action);
        if (!have_action && !local_tx_queued)
        {
            waited_for_action = true;
            have_action = arq_wait_dequeue_action(&action, ARQ_ACTION_WAIT_MS);
        }

        if (have_action)
        {
            cbuf_handle_t action_buffer = NULL;
            size_t action_frame_size = payload_bytes_per_modem_frame;
            if (action.mode >= 0 &&
                arq_policy_ready)
                maybe_switch_modem_mode(modem, action.mode, RX, true);

            pthread_mutex_lock(&modem_freedv_lock);
            action_frame_size = modem->payload_bytes_per_modem_frame;
            pthread_mutex_unlock(&modem_freedv_lock);

            if (action.type == ARQ_ACTION_TX_CONTROL)
                action_buffer = data_tx_buffer_arq_control;
            else if (action.type == ARQ_ACTION_TX_PAYLOAD)
                action_buffer = data_tx_buffer_arq;
            else if (action.type == ARQ_ACTION_MODE_SWITCH)
                sent_from_action = true;

            if (action_buffer &&
                action_frame_size > 0 &&
                action_frame_size <= INT_BUFFER_SIZE &&
                action.frame_size == action_frame_size &&
                size_buffer(action_buffer) >= action_frame_size)
            {
                if (data_size < action_frame_size)
                {
                    uint8_t *new_data = (uint8_t *)realloc(data, action_frame_size);
                    if (!new_data)
                    {
                        HLOGE("modem-tx", "Failed to allocate memory for action TX data");
                        continue;
                    }
                    data = new_data;
                    data_size = action_frame_size;
                }
                read_buffer(action_buffer, data, action_frame_size);
                send_modulated_data(modem, data, 1);
                sent_from_action = true;
            }
        }

        cbuf_handle_t arq_tx_buffer = (payload_bytes_per_modem_frame == 14) ? data_tx_buffer_arq_control : data_tx_buffer_arq;
        if (!sent_from_action && size_buffer(arq_tx_buffer) >= required)
        {
            for (int i = 0; i < tx_frames_per_burst; i++)
            {
                read_buffer(arq_tx_buffer, data + (payload_bytes_per_modem_frame * i), payload_bytes_per_modem_frame);
            }
            send_modulated_data(modem, data, tx_frames_per_burst);
        }

        if (size_buffer(data_tx_buffer_broadcast) >= required)
        {
            for (int i = 0; i < tx_frames_per_burst; i++)
            {
                read_buffer(data_tx_buffer_broadcast, data + (payload_bytes_per_modem_frame * i), payload_bytes_per_modem_frame);
            }
            send_modulated_data(modem, data, tx_frames_per_burst);
        }

        if (!sent_from_action &&
            size_buffer(data_tx_buffer_arq) < required &&
            size_buffer(data_tx_buffer_arq_control) < required &&
            size_buffer(data_tx_buffer_broadcast) < required &&
            !waited_for_action)
        {
            usleep(100000); // sleep for 100ms if there is no data to send
        }
    }

    free(data);
    return NULL;
}

// Function to handle RX logic for the broadcast rx path
void *rx_thread(void *g_modem)
{
    generic_modem_t *modem = (generic_modem_t *)g_modem;
    int32_t *capture_i32 = NULL;
    int16_t *capture_i16 = NULL;
    int capture_cap = 0;
    rx_decoder_state_t control_decoder = {0};
    rx_decoder_state_t payload_decoder = {0};
    int last_pref_rx_mode = -1;
    int last_pref_tx_mode = -1;
    bool was_tx = false;

    while (!shutdown_)
    {
        arq_runtime_snapshot_t arq_snapshot;
        memset(&arq_snapshot, 0, sizeof(arq_snapshot));
        bool have_arq_snapshot = arq_get_runtime_snapshot(&arq_snapshot);
        bool arq_policy_ready = arq_mode_policy_ready_snapshot(have_arq_snapshot, &arq_snapshot);
        int payload_mode = select_payload_rx_mode(&arq_snapshot, arq_policy_ready);
        int pref_rx_mode = -1;
        int pref_tx_mode = -1;
        if (arq_policy_ready)
        {
            pref_rx_mode = arq_snapshot.preferred_rx_mode;
            pref_tx_mode = arq_snapshot.preferred_tx_mode;
            if (pref_rx_mode != last_pref_rx_mode || pref_tx_mode != last_pref_tx_mode)
            {
                HLOGD("modem-rx", "ARQ preferred modes: rx=%d tx=%d", pref_rx_mode, pref_tx_mode);
                last_pref_rx_mode = pref_rx_mode;
                last_pref_tx_mode = pref_tx_mode;
            }

        }

        uint32_t bitrate_bps = 0;
        pthread_mutex_lock(&modem_freedv_lock);
        struct freedv *payload_freedv = pooled_freedv_for_mode_locked(payload_mode, NULL);
        if (payload_freedv)
            bitrate_bps = compute_bitrate_bps_locked(payload_freedv);
        else if (modem->freedv)
            bitrate_bps = compute_bitrate_bps_locked(modem->freedv);
        pthread_mutex_unlock(&modem_freedv_lock);

        if (arq_policy_ready && arq_snapshot.trx == TX)
        {
            // Half-duplex local TX: drain capture at low cost and skip demod work.
            drain_capture_buffer_fast(RX_TX_DRAIN_SAMPLES);
            was_tx = true;
            continue;
        }

        /* PTT just dropped: flush the capture buffer to discard audio that
         * accumulated during TX (drain rate < input rate → backlog builds up).
         * Without this flush, the decoders chew through stale TX-era samples
         * and miss the start of the peer's ACK/reply frame.
         * Also reset the decoder sample accumulators (demod_count=0) so that
         * pre-TX samples queued in demod_in are not mixed with post-TX audio.
         *
         * We intentionally do NOT call freedv_set_sync(UNSYNC) here.
         * The OFDM state machine already transitions to "search" mode after
         * completing or failing a packet (UW_BAD / packet_count == np).
         * Calling UNSYNC zeros the entire rxbuf, which starves the preamble
         * correlation normalizer (mag2 ≈ 0 in est_timing_and_freq) and causes
         * ~50% first-attempt decode failures when the peer's signal arrives
         * before the rxbuf refills with enough real audio (~770ms).
         * With rxbuf left intact (containing harmless old decoded audio or
         * noise floor), the normalizer stays well-conditioned and preamble
         * detection works immediately on real post-TX audio. */
        if (was_tx)
        {
            clear_buffer(capture_buffer);
            control_decoder.demod_count = 0;
            payload_decoder.demod_count = 0;
            was_tx = false;
        }

        if (rx_decoder_bind_mode(&control_decoder, FREEDV_MODE_DATAC13) < 0 ||
            rx_decoder_bind_mode(&payload_decoder, payload_mode) < 0)
        {
            usleep(100000);
            continue;
        }

        int chunk_samples = rx_decoder_target_chunk_samples(&control_decoder);
        int payload_chunk = rx_decoder_target_chunk_samples(&payload_decoder);
        if (payload_chunk > chunk_samples)
            chunk_samples = payload_chunk;

        if (capture_cap < chunk_samples)
        {
            int32_t *new_i32 = (int32_t *)realloc(capture_i32, sizeof(int32_t) * (size_t)chunk_samples);
            int16_t *new_i16 = (int16_t *)realloc(capture_i16, sizeof(int16_t) * (size_t)chunk_samples);
            if (!new_i32 || !new_i16)
            {
                HLOGE("modem-rx", "Failed to allocate dual-decoder capture buffers");
                free(new_i32 ? new_i32 : capture_i32);
                free(new_i16 ? new_i16 : capture_i16);
                capture_i32 = NULL;
                capture_i16 = NULL;
                break;
            }
            capture_i32 = new_i32;
            capture_i16 = new_i16;
            capture_cap = chunk_samples;
        }

        read_buffer(capture_buffer,
                    (uint8_t *)capture_i32,
                    sizeof(int32_t) * (size_t)chunk_samples);
        for (int i = 0; i < chunk_samples; i++)
        {
            capture_i16[i] = (int16_t)(capture_i32[i] >> 16);
        }

        rx_metrics_accum_t metrics = {0};
        rx_decoder_consume_chunk(&control_decoder,
                                 capture_i16,
                                 chunk_samples,
                                 arq_policy_ready,
                                 payload_mode,
                                 bitrate_bps,
                                 &metrics);

        if (payload_decoder.freedv != control_decoder.freedv)
        {
            rx_decoder_consume_chunk(&payload_decoder,
                                     capture_i16,
                                     chunk_samples,
                                     arq_policy_ready,
                                     payload_mode,
                                     bitrate_bps,
                                     &metrics);
        }

        if (arq_policy_ready)
        {
            float snr_est = metrics.snr_valid ? metrics.snr_est : 0.0f;
            arq_update_link_metrics(metrics.sync,
                                    snr_est,
                                    metrics.rx_status,
                                    metrics.frame_decoded);
        }

        /* --- Update spectrum buffer for UI waterfall display --- */
        {
            /* Convert i16 samples to COMP for modem_stats_get_rx_spectrum */
            int spec_nin = chunk_samples;
            if (spec_nin > MODEM_STATS_NSPEC)
                spec_nin = MODEM_STATS_NSPEC;
            COMP rx_fdm[MODEM_STATS_NSPEC];
            for (int i = 0; i < spec_nin; i++)
            {
                /* Pass raw i16 amplitude — modem_stats_get_rx_spectrum normalises
                 * against FDMDV_SCALE=825, not against 32768.  Dividing by 32768
                 * shifts the entire spectrum down by ~90 dB */
                rx_fdm[i].real = (float)capture_i16[i];
                rx_fdm[i].imag = 0.0f;
            }

            pthread_mutex_lock(&g_spectrum_lock);
            if (!g_spectrum_stats_inited)
            {
                modem_stats_open(&g_spectrum_stats);
                g_spectrum_stats_inited = true;
            }
            modem_stats_get_rx_spectrum(&g_spectrum_stats, g_rx_spectrum_dB,
                                        rx_fdm, spec_nin);
            g_spectrum_valid = true;
            /* Determine sample rate from the modem */
            pthread_mutex_lock(&modem_freedv_lock);
            if (modem->freedv)
                g_spectrum_sample_rate = freedv_get_modem_sample_rate(modem->freedv);
            pthread_mutex_unlock(&modem_freedv_lock);
            pthread_mutex_unlock(&g_spectrum_lock);
        }
    }

    rx_decoder_dispose(&control_decoder);
    rx_decoder_dispose(&payload_decoder);
    free(capture_i32);
    free(capture_i16);

    pthread_mutex_lock(&g_spectrum_lock);
    if (g_spectrum_stats_inited)
    {
        modem_stats_close(&g_spectrum_stats);
        g_spectrum_stats_inited = false;
        g_spectrum_valid = false;
    }
    pthread_mutex_unlock(&g_spectrum_lock);

    return NULL;
}

/* --- Public API: get latest rx spectrum for UI waterfall --- */
int modem_get_rx_spectrum(float *out_dB, int max_bins)
{
    int sr = 0;
        /* Validate output buffer and requested number of bins */
    if (out_dB == NULL || max_bins <= 0) {
        return 0;
    }
    pthread_mutex_lock(&g_spectrum_lock);
    if (g_spectrum_valid)
    {
        /* Clamp max_bins to the valid range [0, MODEM_STATS_NSPEC] */
        int n = max_bins;
        if (n < 0) {
            n = 0;
        } else if (n > MODEM_STATS_NSPEC) {
            n = MODEM_STATS_NSPEC;
        }
        if (n > 0)
        {
            memcpy(out_dB, g_rx_spectrum_dB, (size_t)n * sizeof(float));
            sr = g_spectrum_sample_rate;
            g_spectrum_valid = false;
        }
    }
    pthread_mutex_unlock(&g_spectrum_lock);
    return sr;
}
