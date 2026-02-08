/**
 * @file ini_parser.h
 * @brief Simple INI file parser for Mercury settings
 */

#ifndef INI_PARSER_H_
#define INI_PARSER_H_

#include <string>
#include <map>

/**
 * @class IniParser
 * @brief Simple INI file reader/writer
 */
class IniParser {
public:
    IniParser();
    ~IniParser();

    bool load(const std::string& filename);
    bool save(const std::string& filename);

    std::string getString(const std::string& section, const std::string& key,
                          const std::string& defaultValue = "") const;
    int getInt(const std::string& section, const std::string& key, int defaultValue = 0) const;
    double getDouble(const std::string& section, const std::string& key, double defaultValue = 0.0) const;
    bool getBool(const std::string& section, const std::string& key, bool defaultValue = false) const;

    void setString(const std::string& section, const std::string& key, const std::string& value);
    void setInt(const std::string& section, const std::string& key, int value);
    void setDouble(const std::string& section, const std::string& key, double value);
    void setBool(const std::string& section, const std::string& key, bool value);

private:
    std::map<std::string, std::map<std::string, std::string>> data_;
};

/**
 * @brief Get the default config file path
 * @return Path to mercury.ini in user's config directory
 */
std::string getDefaultConfigPath();

/**
 * @struct MercurySettings
 * @brief All Mercury configuration settings
 */
struct MercurySettings {
    // Audio settings
    std::string input_device;
    std::string output_device;
    int input_channel;   // 0=LEFT, 1=RIGHT, 2=STEREO
    int output_channel;
    std::string audio_system;  // "wasapi", "dsound", "alsa", "pulse"

    // Network settings
    int control_port;
    int data_port;

    // Station settings
    std::string my_callsign;
    std::string radio_type;  // "stockhf", "sbitx"

    // ARQ settings
    int connection_timeout_ms;
    int link_timeout_ms;
    int max_connection_attempts;
    bool exit_on_disconnect;

    // PTT Timing settings
    int ptt_on_delay_ms;   // Delay after PTT key before audio starts (radio TX switch time)
    int ptt_off_delay_ms;  // Delay after audio ends before PTT unkey
    int pilot_tone_ms;     // Duration of pilot tone before OFDM (0=disabled)
    int pilot_tone_hz;     // Frequency of pilot tone (250=out of band, 1500=in band)

    // Gear shift settings
    bool gear_shift_enabled;
    int gear_shift_algorithm;
    double gear_shift_up_rate;
    double gear_shift_down_rate;
    int initial_config;
    int ldpc_iterations_max;  // Max LDPC decoder iterations (5-50)

    // Modem settings
    bool coarse_freq_sync_enabled;  // Enable coarse freq search (±30 Hz) for HF radio drift

    // GUI settings
    double tx_gain_db;
    double rx_gain_db;
    bool gains_locked;    // Lock gain sliders to prevent accidental adjustment
    bool hide_console;    // Hide the console window (takes effect on restart)
    int window_width;
    int window_height;

    MercurySettings();
    void setDefaults();
    bool load(const std::string& filename);
    bool save(const std::string& filename);
};

// Global settings - Meyer's Singleton to avoid static init order fiasco
MercurySettings& get_settings();
#define g_settings (get_settings())

#endif // INI_PARSER_H_
