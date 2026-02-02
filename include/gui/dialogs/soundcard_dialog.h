/**
 * @file soundcard_dialog.h
 * @brief Sound card selection dialog
 */

#ifndef SOUNDCARD_DIALOG_H_
#define SOUNDCARD_DIALOG_H_

#include <string>
#include <vector>

/**
 * @struct AudioDeviceInfo
 * @brief Information about an audio device
 */
struct AudioDeviceInfo {
    std::string name;
    std::string id;
    bool is_default;
    int channels;       // Number of channels (1=mono, 2=stereo, 0=unknown)
    int sample_rate;    // Sample rate (0=unknown)
};

/**
 * @class SoundCardDialog
 * @brief Dialog for selecting audio input/output devices
 */
class SoundCardDialog {
public:
    SoundCardDialog();
    ~SoundCardDialog();

    /**
     * @brief Open the dialog
     */
    void open();

    /**
     * @brief Close the dialog
     */
    void close();

    /**
     * @brief Check if dialog is open
     */
    bool isOpen() const { return is_open_; }

    /**
     * @brief Render the dialog (call each frame)
     * @return true if settings were applied
     */
    bool render();

    /**
     * @brief Refresh device lists
     */
    void refreshDevices();

    // Current selections (indices into device lists)
    int selected_input_device_;
    int selected_output_device_;
    int selected_input_channel_;   // 0=LEFT, 1=RIGHT, 2=STEREO
    int selected_output_channel_;
    int selected_audio_system_;    // 0=WASAPI, 1=DirectSound

private:
    bool is_open_;
    bool devices_enumerated_;

    std::vector<AudioDeviceInfo> input_devices_;
    std::vector<AudioDeviceInfo> output_devices_;

    // Temporary selections (before Apply)
    int temp_input_device_;
    int temp_output_device_;
    int temp_input_channel_;
    int temp_output_channel_;
    int temp_audio_system_;
};

// Global dialog - Meyer's Singleton to avoid static init order fiasco
SoundCardDialog& get_soundcard_dialog();
#define g_soundcard_dialog (get_soundcard_dialog())

#endif // SOUNDCARD_DIALOG_H_
