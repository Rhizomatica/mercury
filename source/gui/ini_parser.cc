/**
 * @file ini_parser.cc
 * @brief Simple INI file parser for Mercury settings
 */

#include "gui/ini_parser.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cstdlib>
#include <cstdio>

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#else
#include <unistd.h>
#include <sys/stat.h>
#include <pwd.h>
#endif

// Trim whitespace from string
static std::string trim(const std::string& str) {
    size_t first = str.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    size_t last = str.find_last_not_of(" \t\r\n");
    return str.substr(first, last - first + 1);
}

IniParser::IniParser() {
}

IniParser::~IniParser() {
}

bool IniParser::load(const std::string& filename) {
    // Use C-style fopen instead of ifstream to avoid potential C++ runtime issues
    FILE* fp = fopen(filename.c_str(), "r");
    if (!fp) {
        return false;
    }

    data_.clear();
    std::string current_section = "General";
    char line_buf[1024];

    while (fgets(line_buf, sizeof(line_buf), fp)) {
        std::string line = trim(line_buf);

        // Skip empty lines and comments
        if (line.empty() || line[0] == ';' || line[0] == '#') {
            continue;
        }

        // Section header
        if (line[0] == '[' && line.back() == ']') {
            current_section = line.substr(1, line.length() - 2);
            continue;
        }

        // Key=Value pair
        size_t eq_pos = line.find('=');
        if (eq_pos != std::string::npos) {
            std::string key = trim(line.substr(0, eq_pos));
            std::string value = trim(line.substr(eq_pos + 1));
            data_[current_section][key] = value;
        }
    }

    fclose(fp);
    return true;
}

bool IniParser::save(const std::string& filename) {
    std::ofstream file(filename);
    if (!file.is_open()) {
        return false;
    }

    for (const auto& section : data_) {
        file << "[" << section.first << "]\n";
        for (const auto& kv : section.second) {
            file << kv.first << "=" << kv.second << "\n";
        }
        file << "\n";
    }

    file.close();
    return true;
}

std::string IniParser::getString(const std::string& section, const std::string& key,
                                  const std::string& defaultValue) const {
    auto sec_it = data_.find(section);
    if (sec_it == data_.end()) return defaultValue;

    auto key_it = sec_it->second.find(key);
    if (key_it == sec_it->second.end()) return defaultValue;

    return key_it->second;
}

int IniParser::getInt(const std::string& section, const std::string& key, int defaultValue) const {
    std::string val = getString(section, key, "");
    if (val.empty()) return defaultValue;
    return std::atoi(val.c_str());
}

double IniParser::getDouble(const std::string& section, const std::string& key, double defaultValue) const {
    std::string val = getString(section, key, "");
    if (val.empty()) return defaultValue;
    return std::atof(val.c_str());
}

bool IniParser::getBool(const std::string& section, const std::string& key, bool defaultValue) const {
    std::string val = getString(section, key, "");
    if (val.empty()) return defaultValue;

    // Convert to lowercase for comparison
    std::transform(val.begin(), val.end(), val.begin(), ::tolower);
    return (val == "true" || val == "yes" || val == "1" || val == "on");
}

void IniParser::setString(const std::string& section, const std::string& key, const std::string& value) {
    data_[section][key] = value;
}

void IniParser::setInt(const std::string& section, const std::string& key, int value) {
    data_[section][key] = std::to_string(value);
}

void IniParser::setDouble(const std::string& section, const std::string& key, double value) {
    std::ostringstream oss;
    oss << value;
    data_[section][key] = oss.str();
}

void IniParser::setBool(const std::string& section, const std::string& key, bool value) {
    data_[section][key] = value ? "true" : "false";
}

// Get default config path
std::string getDefaultConfigPath() {
#ifdef _WIN32
    char path[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_APPDATA, NULL, 0, path))) {
        std::string config_dir = std::string(path) + "\\Mercury";
        CreateDirectoryA(config_dir.c_str(), NULL);
        return config_dir + "\\mercury.ini";
    }
    return "mercury.ini";
#else
    const char* home = getenv("HOME");
    if (!home) {
        struct passwd* pw = getpwuid(getuid());
        home = pw->pw_dir;
    }
    std::string config_dir = std::string(home) + "/.config/mercury";
    mkdir(config_dir.c_str(), 0755);
    return config_dir + "/mercury.ini";
#endif
}

// Global settings - Meyer's Singleton to avoid static init order fiasco
MercurySettings& get_settings() {
    static MercurySettings instance;
    return instance;
}

MercurySettings::MercurySettings() {
    setDefaults();
}

void MercurySettings::setDefaults() {
    // Audio
    input_device = "";
    output_device = "";
    input_channel = 0;  // LEFT (matches pre-GUI default for SBITX/unset radio_type)
    output_channel = 2; // STEREO for output
    audio_system = "wasapi";

    // Network
    control_port = 7001;
    data_port = 7002;

    // Station
    my_callsign = "";
    radio_type = "stockhf";

    // ARQ
    connection_timeout_ms = 15000;
    link_timeout_ms = 30000;
    max_connection_attempts = 15;
    exit_on_disconnect = false;

    // PTT Timing
    ptt_on_delay_ms = 100;   // 100ms default for radio TX switch time
    ptt_off_delay_ms = 200;  // 200ms default tail delay
    pilot_tone_ms = 0;       // Disabled by default (0=disabled)
    pilot_tone_hz = 250;     // 250Hz = out of OFDM band, won't interfere with decoder

    // Gear Shift
    gear_shift_enabled = false;
    gear_shift_algorithm = 1;
    gear_shift_up_rate = 70.0;
    gear_shift_down_rate = 40.0;
    initial_config = 1;
    ldpc_iterations_max = 50;

    // Modem
    coarse_freq_sync_enabled = false;  // Off by default (saves CPU; enable for real HF radio use)
    robust_mode_enabled = false;

    // GUI
    tx_gain_db = 0.0;
    rx_gain_db = 0.0;
    gains_locked = true;  // Default to locked to prevent accidental adjustment
    hide_console = false; // Show console by default
    window_width = 850;
    window_height = 650;
}

bool MercurySettings::load(const std::string& filename) {
    IniParser ini;
    if (!ini.load(filename)) {
        return false;
    }

    // Audio
    input_device = ini.getString("Audio", "InputDevice", input_device);
    output_device = ini.getString("Audio", "OutputDevice", output_device);
    input_channel = ini.getInt("Audio", "InputChannel", input_channel);
    output_channel = ini.getInt("Audio", "OutputChannel", output_channel);
    audio_system = ini.getString("Audio", "AudioSystem", audio_system);

    // Network
    control_port = ini.getInt("Network", "ControlPort", control_port);
    data_port = ini.getInt("Network", "DataPort", data_port);

    // Station
    my_callsign = ini.getString("Station", "MyCallsign", my_callsign);
    radio_type = ini.getString("Station", "RadioType", radio_type);

    // ARQ
    connection_timeout_ms = ini.getInt("ARQ", "ConnectionTimeout", connection_timeout_ms);
    link_timeout_ms = ini.getInt("ARQ", "LinkTimeout", link_timeout_ms);
    max_connection_attempts = ini.getInt("ARQ", "MaxConnectionAttempts", max_connection_attempts);
    exit_on_disconnect = ini.getBool("ARQ", "ExitOnDisconnect", exit_on_disconnect);

    // PTT Timing
    ptt_on_delay_ms = ini.getInt("PTT", "OnDelayMs", ptt_on_delay_ms);
    ptt_off_delay_ms = ini.getInt("PTT", "OffDelayMs", ptt_off_delay_ms);
    pilot_tone_ms = ini.getInt("PTT", "PilotToneMs", pilot_tone_ms);
    pilot_tone_hz = ini.getInt("PTT", "PilotToneHz", pilot_tone_hz);

    // Gear Shift
    gear_shift_enabled = ini.getBool("GearShift", "Enabled", gear_shift_enabled);
    gear_shift_algorithm = ini.getInt("GearShift", "Algorithm", gear_shift_algorithm);
    gear_shift_up_rate = ini.getDouble("GearShift", "UpSuccessRate", gear_shift_up_rate);
    gear_shift_down_rate = ini.getDouble("GearShift", "DownSuccessRate", gear_shift_down_rate);
    initial_config = ini.getInt("GearShift", "InitialConfig", initial_config);
    ldpc_iterations_max = ini.getInt("GearShift", "LDPCIterationsMax", ldpc_iterations_max);

    // Modem
    coarse_freq_sync_enabled = ini.getBool("Modem", "CoarseFreqSync", coarse_freq_sync_enabled);
    robust_mode_enabled = ini.getBool("Modem", "RobustMode", robust_mode_enabled);

    // GUI
    tx_gain_db = ini.getDouble("GUI", "TxGainDb", tx_gain_db);
    rx_gain_db = ini.getDouble("GUI", "RxGainDb", rx_gain_db);
    gains_locked = ini.getBool("GUI", "GainsLocked", gains_locked);
    hide_console = ini.getBool("GUI", "HideConsole", hide_console);
    window_width = ini.getInt("GUI", "WindowWidth", window_width);
    window_height = ini.getInt("GUI", "WindowHeight", window_height);

    return true;
}

bool MercurySettings::save(const std::string& filename) {
    IniParser ini;

    // Audio
    ini.setString("Audio", "InputDevice", input_device);
    ini.setString("Audio", "OutputDevice", output_device);
    ini.setInt("Audio", "InputChannel", input_channel);
    ini.setInt("Audio", "OutputChannel", output_channel);
    ini.setString("Audio", "AudioSystem", audio_system);

    // Network
    ini.setInt("Network", "ControlPort", control_port);
    ini.setInt("Network", "DataPort", data_port);

    // Station
    ini.setString("Station", "MyCallsign", my_callsign);
    ini.setString("Station", "RadioType", radio_type);

    // ARQ
    ini.setInt("ARQ", "ConnectionTimeout", connection_timeout_ms);
    ini.setInt("ARQ", "LinkTimeout", link_timeout_ms);
    ini.setInt("ARQ", "MaxConnectionAttempts", max_connection_attempts);
    ini.setBool("ARQ", "ExitOnDisconnect", exit_on_disconnect);

    // PTT Timing
    ini.setInt("PTT", "OnDelayMs", ptt_on_delay_ms);
    ini.setInt("PTT", "OffDelayMs", ptt_off_delay_ms);
    ini.setInt("PTT", "PilotToneMs", pilot_tone_ms);
    ini.setInt("PTT", "PilotToneHz", pilot_tone_hz);

    // Gear Shift
    ini.setBool("GearShift", "Enabled", gear_shift_enabled);
    ini.setInt("GearShift", "Algorithm", gear_shift_algorithm);
    ini.setDouble("GearShift", "UpSuccessRate", gear_shift_up_rate);
    ini.setDouble("GearShift", "DownSuccessRate", gear_shift_down_rate);
    ini.setInt("GearShift", "InitialConfig", initial_config);
    ini.setInt("GearShift", "LDPCIterationsMax", ldpc_iterations_max);

    // Modem
    ini.setBool("Modem", "CoarseFreqSync", coarse_freq_sync_enabled);
    ini.setBool("Modem", "RobustMode", robust_mode_enabled);

    // GUI
    ini.setDouble("GUI", "TxGainDb", tx_gain_db);
    ini.setDouble("GUI", "RxGainDb", rx_gain_db);
    ini.setBool("GUI", "GainsLocked", gains_locked);
    ini.setBool("GUI", "HideConsole", hide_console);
    ini.setInt("GUI", "WindowWidth", window_width);
    ini.setInt("GUI", "WindowHeight", window_height);

    return ini.save(filename);
}
