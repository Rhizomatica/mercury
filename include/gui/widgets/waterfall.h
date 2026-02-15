/**
 * @file waterfall.h
 * @brief Waterfall spectrum display widget
 */

#ifndef WATERFALL_H_
#define WATERFALL_H_

#include <complex>

#define WATERFALL_FFT_SIZE 4096
#define WATERFALL_HISTORY_LINES 500
#define WATERFALL_SAMPLE_RATE 48000
#define WATERFALL_DISPLAY_MAX_HZ 3000  // Display 0 to 3000 Hz
// Bins to display = DISPLAY_MAX_HZ / (SAMPLE_RATE / FFT_SIZE) = 3000 / 11.72 = 256
#define WATERFALL_DISPLAY_BINS 256

/**
 * @class WaterfallDisplay
 * @brief Real-time spectrum waterfall display with OpenGL texture
 */
class WaterfallDisplay {
public:
    WaterfallDisplay();
    ~WaterfallDisplay();

    /**
     * @brief Initialize OpenGL resources
     * @return true on success
     */
    bool init();

    /**
     * @brief Shutdown and free resources
     */
    void shutdown();

    /**
     * @brief Push audio samples for FFT processing
     * @param samples Audio samples (double, -1.0 to 1.0)
     * @param count Number of samples
     */
    void pushSamples(const double* samples, int count);

    /**
     * @brief Render the waterfall display
     * @param width Display width in pixels
     * @param height Display height in pixels
     */
    void render(float width, float height);

    /**
     * @brief Set display parameters
     * @param min_db Minimum dB value (e.g., -100)
     * @param max_db Maximum dB value (e.g., 0)
     */
    void setRange(float min_db, float max_db);

    /**
     * @brief Enable/disable waterfall processing
     * @param enabled When false, pushSamples() skips FFT computation entirely
     */
    void setEnabled(bool enabled);

    /**
     * @brief Check if waterfall processing is enabled
     */
    bool isEnabled() const { return enabled_; }

private:
    // FFT processing
    void processFFT();
    void fft(std::complex<double>* v, int n);

    // Sample buffer for FFT
    double sample_buffer_[WATERFALL_FFT_SIZE];
    int sample_index_;

    // FFT output and history
    float fft_magnitudes_[WATERFALL_FFT_SIZE / 2];  // Full FFT output
    float history_[WATERFALL_HISTORY_LINES][WATERFALL_DISPLAY_BINS];  // Only store display range
    int history_index_;

    // Display parameters
    float min_db_;
    float max_db_;

    // OpenGL texture
    unsigned int texture_id_;
    unsigned char* texture_data_;
    bool initialized_;

    // Processing enabled flag (when false, pushSamples skips FFT)
    bool enabled_;
};

// Global waterfall - Meyer's Singleton to avoid static init order fiasco
WaterfallDisplay& get_waterfall();
#define g_waterfall (get_waterfall())

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Push samples to the global waterfall (thread-safe wrapper)
 */
void waterfall_push_samples(const double* samples, int count);

#ifdef __cplusplus
}
#endif

#endif // WATERFALL_H_
