/**
 * @file waterfall.cc
 * @brief Waterfall spectrum display implementation
 */

#include "gui/widgets/waterfall.h"
#include "gui/gui_state.h"
#include "imgui.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <GL/gl.h>
#include <cmath>
#include <cstring>
#include <cstdio>

// Global waterfall - Meyer's Singleton to avoid static init order fiasco
WaterfallDisplay& get_waterfall() {
    static WaterfallDisplay instance;
    return instance;
}

// Mutex for thread-safe sample pushing - also lazy init
static GuiMutex& get_waterfall_mutex() {
    static GuiMutex instance;
    return instance;
}
#define waterfall_mutex (get_waterfall_mutex())

WaterfallDisplay::WaterfallDisplay()
    : sample_index_(0)
    , history_index_(0)
    , min_db_(-60.0f)
    , max_db_(0.0f)
    , texture_id_(0)
    , texture_data_(nullptr)
    , initialized_(false)
{
    memset(sample_buffer_, 0, sizeof(sample_buffer_));
    memset(fft_magnitudes_, 0, sizeof(fft_magnitudes_));
    for (int i = 0; i < WATERFALL_HISTORY_LINES; i++) {
        for (int j = 0; j < WATERFALL_DISPLAY_BINS; j++) {
            history_[i][j] = min_db_;
        }
    }
}

WaterfallDisplay::~WaterfallDisplay() {
    shutdown();
}

bool WaterfallDisplay::init() {
    if (initialized_) return true;

    // Allocate texture data (RGB, width=DISPLAY_BINS, height=HISTORY_LINES)
    int width = WATERFALL_DISPLAY_BINS;
    int height = WATERFALL_HISTORY_LINES;
    texture_data_ = new unsigned char[width * height * 3];
    memset(texture_data_, 0, width * height * 3);

    // Create OpenGL texture
    glGenTextures(1, &texture_id_);
    glBindTexture(GL_TEXTURE_2D, texture_id_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, width, height, 0, GL_RGB, GL_UNSIGNED_BYTE, texture_data_);

    initialized_ = true;
    return true;
}

void WaterfallDisplay::shutdown() {
    if (texture_id_) {
        glDeleteTextures(1, &texture_id_);
        texture_id_ = 0;
    }
    if (texture_data_) {
        delete[] texture_data_;
        texture_data_ = nullptr;
    }
    initialized_ = false;
}

void WaterfallDisplay::pushSamples(const double* samples, int count) {
    GuiLockGuard lock(waterfall_mutex);

    for (int i = 0; i < count; i++) {
        sample_buffer_[sample_index_] = samples[i];
        sample_index_++;

        // When buffer is full, process FFT
        if (sample_index_ >= WATERFALL_FFT_SIZE) {
            processFFT();
            sample_index_ = 0;
        }
    }
}

void WaterfallDisplay::processFFT() {
    // Copy samples to complex array
    std::complex<double> fft_data[WATERFALL_FFT_SIZE];
    for (int i = 0; i < WATERFALL_FFT_SIZE; i++) {
        // Apply Hanning window
        double window = 0.5 * (1.0 - cos(2.0 * 3.14159265358979 * i / (WATERFALL_FFT_SIZE - 1)));
        fft_data[i] = std::complex<double>(sample_buffer_[i] * window, 0.0);
    }

    // Perform FFT
    fft(fft_data, WATERFALL_FFT_SIZE);

    // Calculate magnitudes in dB (only positive frequencies)
    for (int i = 0; i < WATERFALL_FFT_SIZE / 2; i++) {
        double mag = std::abs(fft_data[i]) / (WATERFALL_FFT_SIZE / 2);
        if (mag < 1e-10) mag = 1e-10;
        fft_magnitudes_[i] = (float)(20.0 * log10(mag));
    }

    // Store in history (only the display bins: 0 to DISPLAY_MAX_HZ)
    for (int i = 0; i < WATERFALL_DISPLAY_BINS; i++) {
        history_[history_index_][i] = fft_magnitudes_[i];
    }
    history_index_ = (history_index_ + 1) % WATERFALL_HISTORY_LINES;
}

// Cooley-Tukey FFT (in-place, radix-2, DIT)
void WaterfallDisplay::fft(std::complex<double>* v, int n) {
    if (n <= 1) return;

    // Separate even and odd
    std::complex<double>* tmp = new std::complex<double>[n];
    for (int i = 0; i < n / 2; i++) {
        tmp[i] = v[2 * i];           // even
        tmp[n / 2 + i] = v[2 * i + 1]; // odd
    }

    // Recurse
    fft(tmp, n / 2);
    fft(tmp + n / 2, n / 2);

    // Combine
    for (int k = 0; k < n / 2; k++) {
        double angle = -2.0 * 3.14159265358979 * k / n;
        std::complex<double> w(cos(angle), sin(angle));
        std::complex<double> t = w * tmp[n / 2 + k];
        v[k] = tmp[k] + t;
        v[k + n / 2] = tmp[k] - t;
    }

    delete[] tmp;
}

// Jet colormap: blue -> cyan -> green -> yellow -> red
static void jetColormap(float value, unsigned char* rgb) {
    // value should be 0.0 to 1.0
    if (value < 0.0f) value = 0.0f;
    if (value > 1.0f) value = 1.0f;

    float r, g, b;
    if (value < 0.25f) {
        r = 0.0f;
        g = 4.0f * value;
        b = 1.0f;
    } else if (value < 0.5f) {
        r = 0.0f;
        g = 1.0f;
        b = 1.0f - 4.0f * (value - 0.25f);
    } else if (value < 0.75f) {
        r = 4.0f * (value - 0.5f);
        g = 1.0f;
        b = 0.0f;
    } else {
        r = 1.0f;
        g = 1.0f - 4.0f * (value - 0.75f);
        b = 0.0f;
    }

    rgb[0] = (unsigned char)(r * 255);
    rgb[1] = (unsigned char)(g * 255);
    rgb[2] = (unsigned char)(b * 255);
}

void WaterfallDisplay::render(float width, float height) {
    if (!initialized_) {
        init();
    }

    int tex_width = WATERFALL_DISPLAY_BINS;
    int tex_height = WATERFALL_HISTORY_LINES;

    // Update texture data from history
    {
        GuiLockGuard lock(waterfall_mutex);

        for (int y = 0; y < tex_height; y++) {
            // Map y to history index (scroll effect)
            int hist_idx = (history_index_ - 1 - y + WATERFALL_HISTORY_LINES) % WATERFALL_HISTORY_LINES;

            for (int x = 0; x < tex_width; x++) {
                float db = history_[hist_idx][x];
                float normalized = (db - min_db_) / (max_db_ - min_db_);

                unsigned char* pixel = &texture_data_[(y * tex_width + x) * 3];
                jetColormap(normalized, pixel);
            }
        }
    }

    // Upload to GPU
    glBindTexture(GL_TEXTURE_2D, texture_id_);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, tex_width, tex_height, GL_RGB, GL_UNSIGNED_BYTE, texture_data_);

    // Draw using ImGui
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImDrawList* draw_list = ImGui::GetWindowDrawList();

    // Draw the texture
    draw_list->AddImage(
        (ImTextureID)(intptr_t)texture_id_,
        pos,
        ImVec2(pos.x + width, pos.y + height),
        ImVec2(0, 0),
        ImVec2(1, 1)
    );

    // Draw frequency scale (0 to WATERFALL_DISPLAY_MAX_HZ)
    char label_mid[32], label_max[32];
    snprintf(label_mid, sizeof(label_mid), "%d Hz", WATERFALL_DISPLAY_MAX_HZ / 2);
    snprintf(label_max, sizeof(label_max), "%d Hz", WATERFALL_DISPLAY_MAX_HZ);

    draw_list->AddText(ImVec2(pos.x, pos.y + height + 2), IM_COL32(200, 200, 200, 255), "0 Hz");
    draw_list->AddText(ImVec2(pos.x + width / 2 - 20, pos.y + height + 2), IM_COL32(200, 200, 200, 255), label_mid);
    draw_list->AddText(ImVec2(pos.x + width - 45, pos.y + height + 2), IM_COL32(200, 200, 200, 255), label_max);

    // Reserve space
    ImGui::Dummy(ImVec2(width, height + 20));
}

void WaterfallDisplay::setRange(float min_db, float max_db) {
    min_db_ = min_db;
    max_db_ = max_db;
}

// Thread-safe wrapper for pushing samples (extern "C" for C linkage)
extern "C" void waterfall_push_samples(const double* samples, int count) {
    g_waterfall.pushSamples(samples, count);
}
