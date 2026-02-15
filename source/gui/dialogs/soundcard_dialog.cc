/**
 * @file soundcard_dialog.cc
 * @brief Sound card selection dialog implementation
 */

#include "gui/dialogs/soundcard_dialog.h"
#include "gui/ini_parser.h"
#include "gui/gui_state.h"
#include "imgui.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#include <climits>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#endif
#include <cstdlib>
#include <cstring>

// ffaudio for device enumeration
extern "C" {
#include "ffaudio/audio.h"
#ifdef _WIN32
extern const ffaudio_interface ffwasapi;
extern const ffaudio_interface ffdsound;
#elif defined(__APPLE__)
extern const ffaudio_interface ffcoreaudio;
#else  // Linux
extern const ffaudio_interface ffalsa;
extern const ffaudio_interface ffpulse;
#endif
}

// Global dialog - Meyer's Singleton to avoid static init order fiasco
SoundCardDialog& get_soundcard_dialog() {
    static SoundCardDialog instance;
    return instance;
}

// Helper to restart Mercury
static void restartMercury() {
    // Save settings before restart
    std::string config_path = getDefaultConfigPath();
    g_settings.save(config_path);

#ifdef _WIN32
    // Get the path to the current executable
    char exePath[MAX_PATH];
    GetModuleFileNameA(NULL, exePath, MAX_PATH);

    // Launch new instance
    STARTUPINFOA si = {sizeof(si)};
    PROCESS_INFORMATION pi;
    if (CreateProcessA(exePath, NULL, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        g_gui_state.request_shutdown.store(true);
    }
#elif defined(__APPLE__)
    // macOS: use _NSGetExecutablePath
    char exePath[PATH_MAX];
    uint32_t size = sizeof(exePath);
    if (_NSGetExecutablePath(exePath, &size) == 0) {
        pid_t pid = fork();
        if (pid == 0) {
            char* argv[] = { exePath, NULL };
            execv(exePath, argv);
            _exit(1);
        } else if (pid > 0) {
            g_gui_state.request_shutdown.store(true);
        }
    }
#else
    // Linux: get executable path via /proc/self/exe
    char exePath[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", exePath, sizeof(exePath) - 1);
    if (len > 0) {
        exePath[len] = '\0';
        pid_t pid = fork();
        if (pid == 0) {
            char* argv[] = { exePath, NULL };
            execv(exePath, argv);
            _exit(1);
        } else if (pid > 0) {
            g_gui_state.request_shutdown.store(true);
        }
    }
#endif
}

SoundCardDialog::SoundCardDialog()
    : is_open_(false)
    , devices_enumerated_(false)
    , selected_input_device_(0)
    , selected_output_device_(0)
    , selected_input_channel_(0)
    , selected_output_channel_(0)
    , selected_audio_system_(0)
    , temp_input_device_(0)
    , temp_output_device_(0)
    , temp_input_channel_(0)
    , temp_output_channel_(0)
    , temp_audio_system_(0)
{
}

SoundCardDialog::~SoundCardDialog() {
}

void SoundCardDialog::open() {
    is_open_ = true;

    // Load settings from g_settings
#ifdef _WIN32
    temp_audio_system_ = (g_settings.audio_system == "wasapi") ? 0 : 1;
#elif defined(__APPLE__)
    temp_audio_system_ = 0;  // CoreAudio only
#else  // Linux
    temp_audio_system_ = (g_settings.audio_system == "alsa") ? 0 : 1;
#endif
    temp_input_channel_ = g_settings.input_channel;
    temp_output_channel_ = g_settings.output_channel;

    // Always refresh devices when opening to ensure we have the current list
    devices_enumerated_ = false;
    refreshDevices();

    // Find the matching input device by name
    temp_input_device_ = 0;  // Default to first device
    for (size_t i = 0; i < input_devices_.size(); i++) {
        if (input_devices_[i].name == g_settings.input_device) {
            temp_input_device_ = (int)i;
            break;
        }
    }

    // Find the matching output device by name
    temp_output_device_ = 0;  // Default to first device
    for (size_t i = 0; i < output_devices_.size(); i++) {
        if (output_devices_[i].name == g_settings.output_device) {
            temp_output_device_ = (int)i;
            break;
        }
    }

    // Sync the "confirmed" selections with temp
    selected_input_device_ = temp_input_device_;
    selected_output_device_ = temp_output_device_;
    selected_input_channel_ = temp_input_channel_;
    selected_output_channel_ = temp_output_channel_;
    selected_audio_system_ = temp_audio_system_;
}

void SoundCardDialog::close() {
    is_open_ = false;
}

void SoundCardDialog::refreshDevices() {
    input_devices_.clear();
    output_devices_.clear();

    // Select audio interface based on current system
#ifdef _WIN32
    const ffaudio_interface* audio = (temp_audio_system_ == 0) ? &ffwasapi : &ffdsound;
#elif defined(__APPLE__)
    const ffaudio_interface* audio = &ffcoreaudio;
#else  // Linux
    const ffaudio_interface* audio = (temp_audio_system_ == 0) ? &ffalsa : &ffpulse;
#endif

    // Initialize audio subsystem
    ffaudio_init_conf init_conf = {};
    init_conf.app_name = "Mercury";
    if (audio->init(&init_conf) != 0) {
        // Add default entries if init fails
        input_devices_.push_back({"Default Input Device", "", true, 0, 0});
        output_devices_.push_back({"Default Output Device", "", true, 0, 0});
        devices_enumerated_ = true;
        return;
    }

    // Enumerate capture (input) devices
    ffaudio_dev* dev = audio->dev_alloc(FFAUDIO_DEV_CAPTURE);
    if (dev) {
        while (audio->dev_next(dev) == 0) {
            AudioDeviceInfo info;
            const char* name = audio->dev_info(dev, FFAUDIO_DEV_NAME);
            const char* id = audio->dev_info(dev, FFAUDIO_DEV_ID);
            const char* is_default = audio->dev_info(dev, FFAUDIO_DEV_IS_DEFAULT);

            info.name = name ? name : "Unknown Device";
            info.id = id ? id : "";
            info.is_default = (is_default != nullptr);
            info.channels = 0;  // Unknown by default
            info.sample_rate = 0;

#ifdef _WIN32
            // Try to get device format info (WASAPI only)
            if (temp_audio_system_ == 0) {
                const char* mix_fmt = audio->dev_info(dev, FFAUDIO_DEV_MIX_FORMAT);
                if (mix_fmt) {
                    const unsigned int* fmt = (const unsigned int*)mix_fmt;
                    info.sample_rate = fmt[1];
                    info.channels = fmt[2];
                }
            }
#endif

            input_devices_.push_back(info);
        }
        audio->dev_free(dev);
    }

    // Enumerate playback (output) devices
    dev = audio->dev_alloc(FFAUDIO_DEV_PLAYBACK);
    if (dev) {
        while (audio->dev_next(dev) == 0) {
            AudioDeviceInfo info;
            const char* name = audio->dev_info(dev, FFAUDIO_DEV_NAME);
            const char* id = audio->dev_info(dev, FFAUDIO_DEV_ID);
            const char* is_default = audio->dev_info(dev, FFAUDIO_DEV_IS_DEFAULT);

            info.name = name ? name : "Unknown Device";
            info.id = id ? id : "";
            info.is_default = (is_default != nullptr);
            info.channels = 0;  // Unknown by default
            info.sample_rate = 0;

#ifdef _WIN32
            // Try to get device format info (WASAPI only)
            if (temp_audio_system_ == 0) {
                const char* mix_fmt = audio->dev_info(dev, FFAUDIO_DEV_MIX_FORMAT);
                if (mix_fmt) {
                    const unsigned int* fmt = (const unsigned int*)mix_fmt;
                    info.sample_rate = fmt[1];
                    info.channels = fmt[2];
                }
            }
#endif

            output_devices_.push_back(info);
        }
        audio->dev_free(dev);
    }

    audio->uninit();

    // Ensure at least one entry exists
    if (input_devices_.empty()) {
        input_devices_.push_back({"Default Input Device", "", true, 0, 0});
    }
    if (output_devices_.empty()) {
        output_devices_.push_back({"Default Output Device", "", true, 0, 0});
    }

    devices_enumerated_ = true;
}

bool SoundCardDialog::render() {
    if (!is_open_) return false;

    bool settings_applied = false;

    ImGui::SetNextWindowSize(ImVec2(500, 400), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Sound Card Settings", &is_open_, ImGuiWindowFlags_NoCollapse)) {

        // Audio System Selection
        ImGui::Text("Audio System:");
        ImGui::SameLine(150);
#ifdef _WIN32
        const char* audio_systems[] = { "WASAPI (Recommended)", "DirectSound" };
        int n_audio_systems = 2;
#elif defined(__APPLE__)
        const char* audio_systems[] = { "CoreAudio" };
        int n_audio_systems = 1;
#else  // Linux
        const char* audio_systems[] = { "ALSA", "PulseAudio" };
        int n_audio_systems = 2;
#endif
        int prev_system = temp_audio_system_;
        if (ImGui::Combo("##audio_system", &temp_audio_system_, audio_systems, n_audio_systems)) {
            if (prev_system != temp_audio_system_) {
                devices_enumerated_ = false;
                refreshDevices();
                temp_input_device_ = 0;
                temp_output_device_ = 0;
            }
        }

        ImGui::Separator();
        ImGui::Spacing();

        // Input Device
        ImGui::Text("Input Device:");
        ImGui::SameLine(150);

        // Build combo items string with device capabilities
        std::string input_combo;
        for (size_t i = 0; i < input_devices_.size(); i++) {
            input_combo += input_devices_[i].name;
            // Show channel info if available
            if (input_devices_[i].channels > 0) {
                input_combo += " [";
                input_combo += (input_devices_[i].channels == 1) ? "Mono" : "Stereo";
                if (input_devices_[i].sample_rate > 0) {
                    input_combo += ", " + std::to_string(input_devices_[i].sample_rate / 1000) + "kHz";
                }
                input_combo += "]";
            }
            if (input_devices_[i].is_default) {
                input_combo += " (Default)";
            }
            input_combo += '\0';
        }
        input_combo += '\0';

        ImGui::SetNextItemWidth(350);
        if (ImGui::Combo("##input_device", &temp_input_device_, input_combo.c_str())) {
            // When device changes, validate channel selection
            if (temp_input_device_ >= 0 && temp_input_device_ < (int)input_devices_.size()) {
                int ch = input_devices_[temp_input_device_].channels;
                if (ch == 1 && temp_input_channel_ != 0) {
                    temp_input_channel_ = 0;  // Force LEFT for mono
                }
            }
        }

        // Input Channel - show options based on device capabilities
        ImGui::Text("Input Channel:");
        ImGui::SameLine(150);

        // Get selected input device channel count
        int input_ch_count = 0;
        if (temp_input_device_ >= 0 && temp_input_device_ < (int)input_devices_.size()) {
            input_ch_count = input_devices_[temp_input_device_].channels;
        }

        ImGui::SetNextItemWidth(150);
        if (input_ch_count == 1) {
            // Mono device - force mono, show as disabled
            const char* mono_options[] = { "Mono (device is mono)" };
            int mono_sel = 0;
            ImGui::Combo("##input_channel", &mono_sel, mono_options, 1);
            temp_input_channel_ = 0;  // Force LEFT (mono)
        } else if (input_ch_count >= 2) {
            // Stereo device - show all options
            const char* channels[] = { "Left", "Right", "Stereo (L+R)" };
            ImGui::Combo("##input_channel", &temp_input_channel_, channels, 3);
        } else {
            // Unknown - show all options with warning
            const char* channels[] = { "Left", "Right", "Stereo (L+R)" };
            ImGui::Combo("##input_channel", &temp_input_channel_, channels, 3);
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.0f, 1.0f), "(?)");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Device channel count unknown.\nIf unsure, try 'Left' for most radio interfaces.");
            }
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Output Device
        ImGui::Text("Output Device:");
        ImGui::SameLine(150);

        std::string output_combo;
        for (size_t i = 0; i < output_devices_.size(); i++) {
            output_combo += output_devices_[i].name;
            // Show channel info if available
            if (output_devices_[i].channels > 0) {
                output_combo += " [";
                output_combo += (output_devices_[i].channels == 1) ? "Mono" : "Stereo";
                if (output_devices_[i].sample_rate > 0) {
                    output_combo += ", " + std::to_string(output_devices_[i].sample_rate / 1000) + "kHz";
                }
                output_combo += "]";
            }
            if (output_devices_[i].is_default) {
                output_combo += " (Default)";
            }
            output_combo += '\0';
        }
        output_combo += '\0';

        ImGui::SetNextItemWidth(350);
        if (ImGui::Combo("##output_device", &temp_output_device_, output_combo.c_str())) {
            // When device changes, validate channel selection
            if (temp_output_device_ >= 0 && temp_output_device_ < (int)output_devices_.size()) {
                int ch = output_devices_[temp_output_device_].channels;
                if (ch == 1) {
                    temp_output_channel_ = 2;  // Force STEREO mode for mono device
                }
            }
        }

        // Output Channel - show options based on device capabilities
        ImGui::Text("Output Channel:");
        ImGui::SameLine(150);

        // Get selected output device channel count
        int output_ch_count = 0;
        if (temp_output_device_ >= 0 && temp_output_device_ < (int)output_devices_.size()) {
            output_ch_count = output_devices_[temp_output_device_].channels;
        }

        ImGui::SetNextItemWidth(150);
        if (output_ch_count == 1) {
            // Mono device - force mono, show as disabled
            const char* mono_options[] = { "Mono (device is mono)" };
            int mono_sel = 0;
            ImGui::Combo("##output_channel", &mono_sel, mono_options, 1);
            temp_output_channel_ = 2;  // Force STEREO mode (will output to mono)
        } else if (output_ch_count >= 2) {
            // Stereo device - show all options
            const char* out_channels[] = { "Left", "Right", "Stereo (Both)" };
            ImGui::Combo("##output_channel", &temp_output_channel_, out_channels, 3);
        } else {
            // Unknown - show all options with warning
            const char* out_channels[] = { "Left", "Right", "Stereo (Both)" };
            ImGui::Combo("##output_channel", &temp_output_channel_, out_channels, 3);
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.0f, 1.0f), "(?)");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Device channel count unknown.\n'Stereo (Both)' is recommended for most setups.");
            }
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Refresh button
        if (ImGui::Button("Refresh Devices")) {
            devices_enumerated_ = false;
            refreshDevices();
        }

        // Info text
        ImGui::Spacing();
#ifdef _WIN32
        ImGui::TextWrapped("WASAPI is recommended for virtual audio cables and provides better latency. "
                           "DirectSound may be more compatible with older hardware.");
#elif defined(__APPLE__)
        ImGui::TextWrapped("CoreAudio is the native macOS audio system.");
#else
        ImGui::TextWrapped("PulseAudio is recommended for desktop Linux. "
                           "ALSA provides direct hardware access with lower latency.");
#endif
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
            "Tip: Most radio USB interfaces are mono. If you see [Mono], channel selection is automatic.");

        ImGui::Spacing();
        ImGui::Spacing();

        // Buttons
        float button_width = 100;
        float total_width = button_width * 3 + ImGui::GetStyle().ItemSpacing.x * 2;
        float start_x = (ImGui::GetWindowWidth() - total_width) / 2;

        ImGui::SetCursorPosX(start_x);
        if (ImGui::Button("OK & Restart", ImVec2(button_width + 20, 0))) {
            // Validate and apply settings
            selected_input_device_ = temp_input_device_;
            selected_output_device_ = temp_output_device_;
            selected_audio_system_ = temp_audio_system_;

            // Validate input channel against device capabilities
            int in_ch_validated = temp_input_channel_;
            if (temp_input_device_ >= 0 && temp_input_device_ < (int)input_devices_.size()) {
                g_settings.input_device = input_devices_[temp_input_device_].name;
                int dev_ch = input_devices_[temp_input_device_].channels;
                if (dev_ch == 1) {
                    in_ch_validated = 0;  // Force LEFT for mono device
                }
            }
            selected_input_channel_ = in_ch_validated;

            // Validate output channel against device capabilities
            int out_ch_validated = temp_output_channel_;
            if (temp_output_device_ >= 0 && temp_output_device_ < (int)output_devices_.size()) {
                g_settings.output_device = output_devices_[temp_output_device_].name;
                int dev_ch = output_devices_[temp_output_device_].channels;
                if (dev_ch == 1) {
                    out_ch_validated = 2;  // Force STEREO mode for mono device (outputs to both/only channel)
                }
            }
            selected_output_channel_ = out_ch_validated;

            g_settings.input_channel = in_ch_validated;
            g_settings.output_channel = out_ch_validated;
#ifdef _WIN32
            g_settings.audio_system = (temp_audio_system_ == 0) ? "wasapi" : "dsound";
#elif defined(__APPLE__)
            g_settings.audio_system = "coreaudio";
#else  // Linux
            g_settings.audio_system = (temp_audio_system_ == 0) ? "alsa" : "pulse";
#endif

            settings_applied = true;
            is_open_ = false;

            // Restart Mercury to apply audio device changes
            restartMercury();
        }

        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(button_width, 0))) {
            is_open_ = false;
        }
    }
    ImGui::End();

    return settings_applied;
}
