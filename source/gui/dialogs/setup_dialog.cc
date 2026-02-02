/**
 * @file setup_dialog.cc
 * @brief Main setup/configuration dialog implementation
 */

#include "gui/dialogs/setup_dialog.h"
#include "gui/ini_parser.h"
#include "gui/gui_state.h"
#include "imgui.h"

#include <cstring>
#include <cstdio>

// Global dialog - Meyer's Singleton to avoid static init order fiasco
SetupDialog& get_setup_dialog() {
    static SetupDialog instance;
    return instance;
}

SetupDialog::SetupDialog()
    : is_open_(false)
    , current_tab_(0)
    , radio_type_(0)
    , control_port_(7001)
    , data_port_(7002)
    , connection_timeout_ms_(60000)
    , link_timeout_ms_(120000)
    , max_connection_attempts_(10)
    , exit_on_disconnect_(false)
    , ptt_on_delay_ms_(100)
    , ptt_off_delay_ms_(200)
    , pilot_tone_ms_(50)
    , pilot_tone_hz_(250)
    , gear_shift_enabled_(true)
    , gear_shift_algorithm_(0)
    , gear_shift_up_rate_(0.8f)
    , gear_shift_down_rate_(0.4f)
    , initial_config_(4)
    , hide_console_(false)
{
    memset(my_callsign_, 0, sizeof(my_callsign_));
    strncpy(my_callsign_, "N0CALL", sizeof(my_callsign_) - 1);
}

SetupDialog::~SetupDialog() {
}

void SetupDialog::open() {
    is_open_ = true;
    loadSettings();
}

void SetupDialog::close() {
    is_open_ = false;
}

void SetupDialog::loadSettings() {
    // Load from global settings
    strncpy(my_callsign_, g_settings.my_callsign.c_str(), sizeof(my_callsign_) - 1);

    if (g_settings.radio_type == "sbitx") {
        radio_type_ = 1;
    } else {
        radio_type_ = 0;
    }

    control_port_ = g_settings.control_port;
    data_port_ = g_settings.data_port;
    connection_timeout_ms_ = g_settings.connection_timeout_ms;
    link_timeout_ms_ = g_settings.link_timeout_ms;
    max_connection_attempts_ = g_settings.max_connection_attempts;
    exit_on_disconnect_ = g_settings.exit_on_disconnect;

    ptt_on_delay_ms_ = g_settings.ptt_on_delay_ms;
    ptt_off_delay_ms_ = g_settings.ptt_off_delay_ms;
    pilot_tone_ms_ = g_settings.pilot_tone_ms;
    pilot_tone_hz_ = g_settings.pilot_tone_hz;

    gear_shift_enabled_ = g_settings.gear_shift_enabled;
    gear_shift_algorithm_ = g_settings.gear_shift_algorithm;
    gear_shift_up_rate_ = (float)g_settings.gear_shift_up_rate;
    gear_shift_down_rate_ = (float)g_settings.gear_shift_down_rate;
    initial_config_ = g_settings.initial_config;

    hide_console_ = g_settings.hide_console;
}

bool SetupDialog::render() {
    if (!is_open_) return false;

    bool settings_applied = false;

    ImGui::SetNextWindowSize(ImVec2(550, 450), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Modem Setup", &is_open_, ImGuiWindowFlags_NoCollapse)) {

        // Tab bar
        if (ImGui::BeginTabBar("SetupTabs")) {

            if (ImGui::BeginTabItem("Station")) {
                renderStationTab();
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Network")) {
                renderNetworkTab();
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("ARQ")) {
                renderARQTab();
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Gear Shift")) {
                renderGearShiftTab();
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Advanced")) {
                renderAdvancedTab();
                ImGui::EndTabItem();
            }

            ImGui::EndTabBar();
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Buttons
        float button_width = 100;
        float total_width = button_width * 3 + ImGui::GetStyle().ItemSpacing.x * 2;
        float start_x = (ImGui::GetWindowWidth() - total_width) / 2;

        ImGui::SetCursorPosX(start_x);
        if (ImGui::Button("OK", ImVec2(button_width, 0))) {
            // Apply all settings
            g_settings.my_callsign = my_callsign_;
            g_settings.radio_type = (radio_type_ == 1) ? "sbitx" : "stockhf";
            g_settings.control_port = control_port_;
            g_settings.data_port = data_port_;
            g_settings.connection_timeout_ms = connection_timeout_ms_;
            g_settings.link_timeout_ms = link_timeout_ms_;
            g_settings.max_connection_attempts = max_connection_attempts_;
            g_settings.exit_on_disconnect = exit_on_disconnect_;
            g_settings.ptt_on_delay_ms = ptt_on_delay_ms_;
            g_settings.ptt_off_delay_ms = ptt_off_delay_ms_;
            g_settings.pilot_tone_ms = pilot_tone_ms_;
            g_settings.pilot_tone_hz = pilot_tone_hz_;
            g_settings.gear_shift_enabled = gear_shift_enabled_;
            g_settings.gear_shift_algorithm = gear_shift_algorithm_;
            g_settings.gear_shift_up_rate = gear_shift_up_rate_;
            g_settings.gear_shift_down_rate = gear_shift_down_rate_;
            g_settings.initial_config = initial_config_;
            g_settings.hide_console = hide_console_;

            settings_applied = true;
            is_open_ = false;
        }

        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(button_width, 0))) {
            is_open_ = false;
        }

        ImGui::SameLine();
        if (ImGui::Button("Apply", ImVec2(button_width, 0))) {
            g_settings.my_callsign = my_callsign_;
            g_settings.radio_type = (radio_type_ == 1) ? "sbitx" : "stockhf";
            g_settings.control_port = control_port_;
            g_settings.data_port = data_port_;
            g_settings.connection_timeout_ms = connection_timeout_ms_;
            g_settings.link_timeout_ms = link_timeout_ms_;
            g_settings.max_connection_attempts = max_connection_attempts_;
            g_settings.exit_on_disconnect = exit_on_disconnect_;
            g_settings.ptt_on_delay_ms = ptt_on_delay_ms_;
            g_settings.ptt_off_delay_ms = ptt_off_delay_ms_;
            g_settings.pilot_tone_ms = pilot_tone_ms_;
            g_settings.pilot_tone_hz = pilot_tone_hz_;
            g_settings.gear_shift_enabled = gear_shift_enabled_;
            g_settings.gear_shift_algorithm = gear_shift_algorithm_;
            g_settings.gear_shift_up_rate = gear_shift_up_rate_;
            g_settings.gear_shift_down_rate = gear_shift_down_rate_;
            g_settings.initial_config = initial_config_;
            g_settings.hide_console = hide_console_;

            settings_applied = true;
        }
    }
    ImGui::End();

    return settings_applied;
}

void SetupDialog::renderStationTab() {
    ImGui::Spacing();

    ImGui::Text("My Callsign:");
    ImGui::SameLine(150);
    ImGui::SetNextItemWidth(150);
    ImGui::InputText("##callsign", my_callsign_, sizeof(my_callsign_),
                     ImGuiInputTextFlags_CharsUppercase | ImGuiInputTextFlags_CharsNoBlank);

    ImGui::Spacing();

    ImGui::Text("Radio Type:");
    ImGui::SameLine(150);
    const char* radio_types[] = { "Stock HF (Generic)", "sBitx" };
    ImGui::SetNextItemWidth(200);
    ImGui::Combo("##radio_type", &radio_type_, radio_types, 2);

    ImGui::Spacing();
    ImGui::Spacing();

    ImGui::TextWrapped("Your callsign is used for the ARQ protocol handshake. "
                       "The radio type affects PTT and CAT control behavior.");
}

void SetupDialog::renderNetworkTab() {
    ImGui::Spacing();

    ImGui::Text("Control Port:");
    ImGui::SameLine(150);
    ImGui::SetNextItemWidth(100);
    ImGui::InputInt("##control_port", &control_port_);
    if (control_port_ < 1024) control_port_ = 1024;
    if (control_port_ > 65535) control_port_ = 65535;

    ImGui::Spacing();

    ImGui::Text("Data Port:");
    ImGui::SameLine(150);
    ImGui::SetNextItemWidth(100);
    ImGui::InputInt("##data_port", &data_port_);
    if (data_port_ < 1024) data_port_ = 1024;
    if (data_port_ > 65535) data_port_ = 65535;

    ImGui::Spacing();
    ImGui::Spacing();

    ImGui::TextWrapped("Control port is used for commands (connect, disconnect, etc.). "
                       "Data port is used for the actual data transfer. "
                       "Default ports are 52001 and 52002.");
}

void SetupDialog::renderARQTab() {
    ImGui::Spacing();

    ImGui::Text("Connection Timeout:");
    ImGui::SameLine(180);
    ImGui::SetNextItemWidth(100);
    ImGui::InputInt("ms##conn_timeout", &connection_timeout_ms_);
    if (connection_timeout_ms_ < 1000) connection_timeout_ms_ = 1000;
    if (connection_timeout_ms_ > 300000) connection_timeout_ms_ = 300000;

    ImGui::Spacing();

    ImGui::Text("Link Timeout:");
    ImGui::SameLine(180);
    ImGui::SetNextItemWidth(100);
    ImGui::InputInt("ms##link_timeout", &link_timeout_ms_);
    if (link_timeout_ms_ < 1000) link_timeout_ms_ = 1000;
    if (link_timeout_ms_ > 600000) link_timeout_ms_ = 600000;

    ImGui::Spacing();

    ImGui::Text("Max Connect Attempts:");
    ImGui::SameLine(180);
    ImGui::SetNextItemWidth(100);
    ImGui::InputInt("##max_attempts", &max_connection_attempts_);
    if (max_connection_attempts_ < 1) max_connection_attempts_ = 1;
    if (max_connection_attempts_ > 100) max_connection_attempts_ = 100;

    ImGui::Spacing();

    ImGui::Checkbox("Exit on Disconnect", &exit_on_disconnect_);

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // PTT Timing Section
    ImGui::Text("PTT Timing");
    ImGui::Spacing();

    ImGui::Text("PTT On Delay:");
    ImGui::SameLine(180);
    ImGui::SetNextItemWidth(100);
    ImGui::InputInt("ms##ptt_on", &ptt_on_delay_ms_);
    if (ptt_on_delay_ms_ < 0) ptt_on_delay_ms_ = 0;
    if (ptt_on_delay_ms_ > 1000) ptt_on_delay_ms_ = 1000;

    ImGui::Spacing();

    ImGui::Text("PTT Off Delay:");
    ImGui::SameLine(180);
    ImGui::SetNextItemWidth(100);
    ImGui::InputInt("ms##ptt_off", &ptt_off_delay_ms_);
    if (ptt_off_delay_ms_ < 0) ptt_off_delay_ms_ = 0;
    if (ptt_off_delay_ms_ > 1000) ptt_off_delay_ms_ = 1000;

    ImGui::Spacing();

    ImGui::Text("Pilot Tone:");
    ImGui::SameLine(180);
    ImGui::SetNextItemWidth(80);
    ImGui::InputInt("ms##pilot", &pilot_tone_ms_);
    if (pilot_tone_ms_ < 0) pilot_tone_ms_ = 0;
    if (pilot_tone_ms_ > 500) pilot_tone_ms_ = 500;

    ImGui::Text("Pilot Freq:");
    ImGui::SameLine(180);
    ImGui::SetNextItemWidth(80);
    ImGui::InputInt("Hz##pilot_freq", &pilot_tone_hz_);
    if (pilot_tone_hz_ < 100) pilot_tone_hz_ = 100;
    if (pilot_tone_hz_ > 3000) pilot_tone_hz_ = 3000;

    ImGui::Spacing();
    ImGui::Spacing();

    ImGui::TextWrapped("PTT On Delay: Time after keying PTT before audio starts. "
                       "Increase if your radio clips the start of transmissions (100-200ms typical).");

    ImGui::Spacing();

    ImGui::TextWrapped("PTT Off Delay: Time after audio ends before unkeying PTT. "
                       "Increase if your transmissions are clipped at the end (200-500ms typical).");

    ImGui::Spacing();

    ImGui::TextWrapped("Pilot Tone: Tone before OFDM to trigger RF-sensing amplifiers. "
                       "Use 250Hz (out of band) to avoid decoder interference, or 1500Hz (in band). "
                       "Set duration to 0 to disable.");

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::TextWrapped("Connection timeout is the max time to wait for a response during connection. "
                       "Link timeout is the max time without receiving data before disconnecting.");
}

void SetupDialog::renderGearShiftTab() {
    ImGui::Spacing();

    ImGui::Checkbox("Enable Gear Shifting", &gear_shift_enabled_);

    ImGui::Spacing();

    if (!gear_shift_enabled_) {
        ImGui::BeginDisabled();
    }

    ImGui::Text("Algorithm:");
    ImGui::SameLine(180);
    const char* algorithms[] = { "Conservative", "Aggressive", "Balanced" };
    ImGui::SetNextItemWidth(150);
    ImGui::Combo("##gs_algorithm", &gear_shift_algorithm_, algorithms, 3);

    ImGui::Spacing();

    ImGui::Text("Upshift Threshold:");
    ImGui::SameLine(180);
    ImGui::SetNextItemWidth(150);
    ImGui::SliderFloat("##gs_up", &gear_shift_up_rate_, 0.5f, 1.0f, "%.2f");

    ImGui::Text("Downshift Threshold:");
    ImGui::SameLine(180);
    ImGui::SetNextItemWidth(150);
    ImGui::SliderFloat("##gs_down", &gear_shift_down_rate_, 0.1f, 0.8f, "%.2f");

    ImGui::Spacing();

    ImGui::Text("Initial Configuration:");
    ImGui::SameLine(180);
    const char* configs[] = { "0 - Lowest", "1", "2", "3", "4 - Default", "5", "6", "7", "8", "9 - Highest" };
    ImGui::SetNextItemWidth(150);
    ImGui::Combo("##initial_config", &initial_config_, configs, 10);

    if (!gear_shift_enabled_) {
        ImGui::EndDisabled();
    }

    ImGui::Spacing();
    ImGui::Spacing();

    ImGui::TextWrapped("Gear shifting automatically adjusts the modulation configuration based on channel conditions. "
                       "Higher configurations provide faster data rates but require better channel quality.");
}

void SetupDialog::renderAdvancedTab() {
    ImGui::Spacing();

    ImGui::Text("Audio Gain Settings");
    ImGui::Separator();
    ImGui::Spacing();

    // Note about main window controls
    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
        "Note: Gain controls are available on the main window.");
    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
        "Use 'Lock Gain Adjustments' checkbox to enable/disable editing.");
    ImGui::Spacing();

    // Show current values (read-only display)
    float tx_gain = (float)g_gui_state.tx_gain_db.load();
    float rx_gain = (float)g_gui_state.rx_gain_db.load();
    bool gains_locked = g_gui_state.gains_locked.load();

    ImGui::Text("TX Drive Level: %.1f dB", tx_gain);
    ImGui::Text("RX Gain: %.1f dB", rx_gain);
    ImGui::Text("Gains Locked: %s", gains_locked ? "Yes" : "No");

    ImGui::Spacing();
    ImGui::Spacing();

    ImGui::TextWrapped("TX Drive Level adjusts the output volume to your radio. "
                       "Increase if your transmission is too quiet, decrease if clipping or ALC is too high.");

    ImGui::Spacing();

    ImGui::TextWrapped("RX Gain adjusts the input sensitivity. "
                       "Adjust with NO signal present to bring the noise floor to 0 dBm on the tuning meter. "
                       "This calibrates your receive levels relative to the noise floor.");

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Console visibility
    ImGui::Text("Window Settings");
    ImGui::Separator();
    ImGui::Spacing();

    if (ImGui::Checkbox("Hide console window", &hide_console_)) {
        // Value changed
    }
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "(requires restart)");

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Save/Load buttons
    if (ImGui::Button("Save Settings to File")) {
        std::string path = getDefaultConfigPath();
        if (g_settings.save(path)) {
            // Success - could show a notification
        }
    }

    ImGui::SameLine();

    if (ImGui::Button("Load Settings from File")) {
        std::string path = getDefaultConfigPath();
        if (g_settings.load(path)) {
            loadSettings();  // Refresh dialog with loaded values
        }
    }
}
