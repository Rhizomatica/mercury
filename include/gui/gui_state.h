/**
 * @file gui_state.h
 * @brief Thread-safe shared state between GUI and Mercury core
 */

#ifndef GUI_STATE_H_
#define GUI_STATE_H_

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

#include <atomic>
#include <complex>
#include <cmath>
#include <cstring>

// Buffer sizes
#define GUI_VU_BUFFER_SIZE 2048
#define GUI_WATERFALL_FFT_SIZE 512
#define GUI_WATERFALL_HISTORY 100
#define GUI_CONSTELLATION_MAX_POINTS 2500  // Nc(50) * max_Nsymb(48) = 2400, rounded up

// Simple cross-platform mutex wrapper
class GuiMutex {
public:
#ifdef _WIN32
    GuiMutex() { InitializeCriticalSection(&cs_); }
    ~GuiMutex() { DeleteCriticalSection(&cs_); }
    void lock() { EnterCriticalSection(&cs_); }
    void unlock() { LeaveCriticalSection(&cs_); }
private:
    CRITICAL_SECTION cs_;
#else
    GuiMutex() { pthread_mutex_init(&mutex_, NULL); }
    ~GuiMutex() { pthread_mutex_destroy(&mutex_); }
    void lock() { pthread_mutex_lock(&mutex_); }
    void unlock() { pthread_mutex_unlock(&mutex_); }
private:
    pthread_mutex_t mutex_;
#endif
};

// RAII lock guard
class GuiLockGuard {
public:
    GuiLockGuard(GuiMutex& m) : mutex_(m) { mutex_.lock(); }
    ~GuiLockGuard() { mutex_.unlock(); }
private:
    GuiMutex& mutex_;
};

/**
 * @struct st_gui_state
 * @brief Shared state between GUI thread and Mercury core threads
 *
 * Atomic values are used for simple read/write access.
 * Mutex-protected sections are used for buffer data.
 */
struct st_gui_state {
    // ========== Receive Statistics (updated by telecom_system) ==========
    std::atomic<double> snr_db{0.0};                  // Raw physical layer SNR (deprecated - use snr_uplink_db)
    std::atomic<double> signal_strength_dbm{-100.0};
    std::atomic<double> freq_offset_hz{0.0};

    // ========== ARQ Measurements (updated by ARQ layer) ==========
    std::atomic<double> snr_uplink_db{-99.9};         // SNR of signals WE receive (local measurement)
    std::atomic<double> snr_downlink_db{-99.9};       // SNR of OUR signals at remote station (reported back)

    // ========== Connection State (updated by ARQ) ==========
    std::atomic<int> link_status{0};          // IDLE=0, CONNECTING=1, CONNECTED=2, etc.
    std::atomic<int> connection_status{0};    // Current activity
    std::atomic<int> role{0};                 // COMMANDER=0, RESPONDER=1
    std::atomic<int> current_configuration{0};
    std::atomic<double> current_bitrate{0.0};

    // ========== TX/RX State ==========
    std::atomic<bool> is_transmitting{false};
    std::atomic<bool> is_receiving{false};
    std::atomic<bool> data_activity{false};
    std::atomic<bool> ack_activity{false};

    // ========== Gain Controls (GUI -> Core) ==========
    std::atomic<double> tx_gain_db{0.0};      // -20 to +6 dB
    std::atomic<double> rx_gain_db{0.0};      // -15 to +35 dB
    std::atomic<bool> tune_active{false};     // Generate continuous 1500 Hz tone
    std::atomic<bool> gains_locked{true};     // Lock gain sliders (default: locked)
    std::atomic<bool> auto_tune_active{false}; // Auto-tune RX gain to target 0 dBm

    // ========== VU Meter Data ==========
    GuiMutex vu_mutex;
    double vu_samples[GUI_VU_BUFFER_SIZE];
    int vu_write_index{0};
    double vu_peak_db{-100.0};
    double vu_rms_db{-100.0};

    // ========== Waterfall Data ==========
    GuiMutex waterfall_mutex;
    float waterfall_fft[GUI_WATERFALL_FFT_SIZE];
    float waterfall_history[GUI_WATERFALL_HISTORY][GUI_WATERFALL_FFT_SIZE];
    int waterfall_history_index{0};

    // ========== Statistics ==========
    std::atomic<int> nSent_data{0};
    std::atomic<int> nAcked_data{0};
    std::atomic<int> nReceived_data{0};
    std::atomic<int> nLost_data{0};
    std::atomic<float> success_rate{0.0f};

    // ========== Callsigns ==========
    GuiMutex callsign_mutex;
    char my_callsign[32]{0};
    char dest_callsign[32]{0};

    // ========== Performance Monitoring ==========
    std::atomic<float> processing_load{0.0f};       // processing_time / frame_period (>1.0 = falling behind)
    std::atomic<float> buffer_fill_pct{0.0f};       // capture ring buffer fill percentage (0-100)
    std::atomic<int> ldpc_iterations_max{50};        // GUI-configurable LDPC max iterations

    // ========== Constellation Data (updated by telecom_system on decode) ==========
    GuiMutex constellation_mutex;
    float constellation_i[GUI_CONSTELLATION_MAX_POINTS];  // In-phase (real) values
    float constellation_q[GUI_CONSTELLATION_MAX_POINTS];  // Quadrature (imag) values
    int constellation_count{0};                           // Number of valid IQ points
    std::atomic<bool> constellation_is_mfsk{false};       // true = MFSK mode, no IQ data
    std::atomic<int> constellation_modulation{2};         // MOD_BPSK=2, QPSK=4, etc.

    // ========== Throughput Tracking (updated by ARQ) ==========
    std::atomic<long long> bytes_acked_total{0};          // Commander: cumulative bytes ACKed
    std::atomic<long long> bytes_received_total{0};       // Responder: cumulative bytes received
    std::atomic<double> throughput_bps{0.0};              // Rolling 10s average throughput (bps)

    // ========== Modem Options ==========
    std::atomic<bool> coarse_freq_sync_enabled{false}; // Coarse freq search (±30 Hz), for HF radio use
    std::atomic<bool> robust_mode_enabled{false};      // MFSK robust mode for weak-signal hailing

    // ========== GUI Control ==========
    std::atomic<bool> gui_running{true};
    std::atomic<bool> request_shutdown{false};

    // Initialize with defaults
    st_gui_state() {
        for (int i = 0; i < GUI_VU_BUFFER_SIZE; i++) {
            vu_samples[i] = 0.0;
        }
        for (int i = 0; i < GUI_WATERFALL_FFT_SIZE; i++) {
            waterfall_fft[i] = -100.0f;
        }
        for (int i = 0; i < GUI_WATERFALL_HISTORY; i++) {
            for (int j = 0; j < GUI_WATERFALL_FFT_SIZE; j++) {
                waterfall_history[i][j] = -100.0f;
            }
        }
        memset(constellation_i, 0, sizeof(constellation_i));
        memset(constellation_q, 0, sizeof(constellation_q));
    }
};

// Global GUI state - Meyer's Singleton to avoid static init order fiasco
st_gui_state& get_gui_state();
#define g_gui_state (get_gui_state())

// ========== Helper Functions ==========

/**
 * @brief Push audio samples to VU meter buffer
 * @param samples Pointer to audio samples (double, -1.0 to 1.0 range)
 * @param count Number of samples
 */
inline void gui_push_vu_samples(const double* samples, int count) {
    GuiLockGuard lock(g_gui_state.vu_mutex);

    double peak = 0.0;
    double sum_sq = 0.0;

    for (int i = 0; i < count; i++) {
        // Store sample in ring buffer
        g_gui_state.vu_samples[g_gui_state.vu_write_index] = samples[i];
        g_gui_state.vu_write_index = (g_gui_state.vu_write_index + 1) % GUI_VU_BUFFER_SIZE;

        // Calculate peak and RMS
        double abs_sample = fabs(samples[i]);
        if (abs_sample > peak) peak = abs_sample;
        sum_sq += samples[i] * samples[i];
    }

    // Update peak (with slight decay if new peak is lower)
    double new_peak_db = (peak > 1e-10) ? 20.0 * log10(peak) : -100.0;
    if (new_peak_db > g_gui_state.vu_peak_db) {
        g_gui_state.vu_peak_db = new_peak_db;
    } else {
        g_gui_state.vu_peak_db -= 0.5; // Decay ~30 dB/sec at 60fps
        if (g_gui_state.vu_peak_db < -100.0) g_gui_state.vu_peak_db = -100.0;
    }

    // Update RMS
    double rms = sqrt(sum_sq / count);
    g_gui_state.vu_rms_db = (rms > 1e-10) ? 20.0 * log10(rms) : -100.0;
}

/**
 * @brief Apply TX gain to output samples
 * @param samples Pointer to audio samples (modified in-place)
 * @param count Number of samples
 */
inline void gui_apply_tx_gain(double* samples, int count) {
    double gain_linear = pow(10.0, g_gui_state.tx_gain_db.load() / 20.0);
    for (int i = 0; i < count; i++) {
        samples[i] *= gain_linear;
    }
}

/**
 * @brief Apply RX gain to input samples (affects core modem processing)
 * Note: Called in audioio capture path before ring buffer write — gain applies
 * to both GUI display AND Mercury's core modem.
 * @param samples Pointer to audio samples (modified in-place)
 * @param count Number of samples
 */
inline void gui_apply_rx_gain_for_display(double* samples, int count) {
    double gain_linear = pow(10.0, g_gui_state.rx_gain_db.load() / 20.0);
    for (int i = 0; i < count; i++) {
        samples[i] *= gain_linear;
    }
}

/**
 * @brief Generate tune tone sample (1500 Hz sine wave)
 * @param sample_rate Audio sample rate (typically 48000)
 * @param sample_index Current sample index (incremented by caller)
 * @return Tone sample value (-1.0 to 1.0)
 */
inline double gui_generate_tune_tone(int sample_rate, long& sample_index) {
    const double TUNE_FREQ = 1500.0;
    double t = (double)sample_index / (double)sample_rate;
    sample_index++;
    return sin(2.0 * 3.14159265358979323846 * TUNE_FREQ * t) * 0.9; // 90% amplitude for VOX
}

/**
 * @brief Update receive statistics from telecom_system
 */
inline void gui_update_receive_stats(double snr, double signal_dbm, double freq_offset) {
    g_gui_state.snr_db.store(snr);
    g_gui_state.signal_strength_dbm.store(signal_dbm);
    g_gui_state.freq_offset_hz.store(freq_offset);
}

/**
 * @brief Update ARQ measurements from arq layer
 * @param snr_uplink SNR of signals we receive (locally measured)
 * @param snr_downlink SNR of our signals at remote station (reported by remote)
 */
inline void gui_update_arq_measurements(double snr_uplink, double snr_downlink) {
    g_gui_state.snr_uplink_db.store(snr_uplink);
    g_gui_state.snr_downlink_db.store(snr_downlink);
}

/**
 * @brief Update connection status from ARQ
 */
inline void gui_update_connection_status(int link_status, int conn_status, int role) {
    g_gui_state.link_status.store(link_status);
    g_gui_state.connection_status.store(conn_status);
    g_gui_state.role.store(role);
}

/**
 * @brief Push samples to waterfall display (forward declaration)
 * Implementation in waterfall.cc
 */
extern "C" void waterfall_push_samples(const double* samples, int count);

/**
 * @brief Push samples to both VU meter and waterfall
 */
inline void gui_push_audio_samples(const double* samples, int count) {
    gui_push_vu_samples(samples, count);
    waterfall_push_samples(samples, count);
}

/**
 * @brief Push constellation (IQ scatter) data from decoded frame
 * @param iq_data Complex IQ symbols after equalization/deframing
 * @param count Number of IQ points
 * @param modulation Modulation type (MOD_BPSK=2, MOD_QPSK=4, etc.)
 * @param is_mfsk true if current mode is MFSK (no IQ data available)
 */
inline void gui_push_constellation(const std::complex<double>* iq_data, int count,
                                    int modulation, bool is_mfsk) {
    g_gui_state.constellation_is_mfsk.store(is_mfsk);
    g_gui_state.constellation_modulation.store(modulation);
    if (is_mfsk || count <= 0 || iq_data == nullptr) return;

    GuiLockGuard lock(g_gui_state.constellation_mutex);
    int n = (count > GUI_CONSTELLATION_MAX_POINTS) ? GUI_CONSTELLATION_MAX_POINTS : count;
    for (int i = 0; i < n; i++) {
        g_gui_state.constellation_i[i] = (float)iq_data[i].real();
        g_gui_state.constellation_q[i] = (float)iq_data[i].imag();
    }
    g_gui_state.constellation_count = n;
}

/**
 * @brief Add bytes to throughput tracking (called from ARQ on data ACK/receive)
 */
inline void gui_add_throughput_bytes_tx(long long bytes) {
    g_gui_state.bytes_acked_total.fetch_add(bytes);
}

inline void gui_add_throughput_bytes_rx(long long bytes) {
    g_gui_state.bytes_received_total.fetch_add(bytes);
}

#endif // GUI_STATE_H_
