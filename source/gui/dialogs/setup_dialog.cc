/**
 * @file setup_dialog.cc
 * @brief Main setup/configuration dialog implementation
 */

#include "gui/dialogs/setup_dialog.h"
#include "gui/ini_parser.h"
#include "gui/gui_state.h"
#include "common/common_defines.h"
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
    , control_port_(7002)
    , data_port_(7003)
    , connection_timeout_ms_(60000)
    , link_timeout_ms_(120000)
    , max_connection_attempts_(10)
    , exit_on_disconnect_(false)
    , ptt_on_delay_ms_(100)
    , ptt_off_delay_ms_(200)
    , pilot_tone_ms_(0)
    , pilot_tone_hz_(250)
    , gear_shift_enabled_(true)
    , initial_config_(4)
    , ldpc_iterations_max_(50)
    , coarse_freq_sync_enabled_(false)
    , robust_mode_enabled_(false)
    , bandwidth_mode_(0)
    , hide_console_(false)
    , encryption_mode_(0)
{
    memset(my_callsign_, 0, sizeof(my_callsign_));
    strncpy(my_callsign_, "N0CALL", sizeof(my_callsign_) - 1);
    memset(psk_hex_, 0, sizeof(psk_hex_));
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
    initial_config_ = g_settings.initial_config;
    ldpc_iterations_max_ = g_settings.ldpc_iterations_max;
    coarse_freq_sync_enabled_ = g_settings.coarse_freq_sync_enabled;
    robust_mode_enabled_ = g_settings.robust_mode_enabled;
    bandwidth_mode_ = g_settings.bandwidth_mode;

    hide_console_ = g_settings.hide_console;

    encryption_mode_ = g_settings.encryption_mode;
    strncpy(psk_hex_, g_settings.psk_hex.c_str(), sizeof(psk_hex_) - 1);
    psk_hex_[sizeof(psk_hex_) - 1] = 0;
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

            if (ImGui::BeginTabItem("Security")) {
                renderSecurityTab();
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
            g_settings.initial_config = initial_config_;
            g_settings.ldpc_iterations_max = ldpc_iterations_max_;
            g_gui_state.ldpc_iterations_max.store(ldpc_iterations_max_);
            g_settings.coarse_freq_sync_enabled = coarse_freq_sync_enabled_;
            g_gui_state.coarse_freq_sync_enabled.store(coarse_freq_sync_enabled_);
            g_settings.robust_mode_enabled = robust_mode_enabled_;
            g_gui_state.robust_mode_enabled.store(robust_mode_enabled_);
            g_settings.bandwidth_mode = bandwidth_mode_;
            g_gui_state.bandwidth_mode.store(bandwidth_mode_);
            // narrowband_enabled driven by bandwidth_mode: always start NB
            g_settings.narrowband_enabled = true;
            g_gui_state.narrowband_enabled.store(true);
            g_settings.hide_console = hide_console_;
            g_settings.encryption_mode = encryption_mode_;
            g_gui_state.encryption_mode.store(encryption_mode_);
            g_settings.psk_hex = psk_hex_;

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
            g_settings.initial_config = initial_config_;
            g_settings.ldpc_iterations_max = ldpc_iterations_max_;
            g_gui_state.ldpc_iterations_max.store(ldpc_iterations_max_);
            g_settings.coarse_freq_sync_enabled = coarse_freq_sync_enabled_;
            g_gui_state.coarse_freq_sync_enabled.store(coarse_freq_sync_enabled_);
            g_settings.robust_mode_enabled = robust_mode_enabled_;
            g_gui_state.robust_mode_enabled.store(robust_mode_enabled_);
            g_settings.bandwidth_mode = bandwidth_mode_;
            g_gui_state.bandwidth_mode.store(bandwidth_mode_);
            // narrowband_enabled driven by bandwidth_mode: always start NB
            g_settings.narrowband_enabled = true;
            g_gui_state.narrowband_enabled.store(true);
            g_settings.hide_console = hide_console_;
            g_settings.encryption_mode = encryption_mode_;
            g_gui_state.encryption_mode.store(encryption_mode_);
            g_settings.psk_hex = psk_hex_;

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
                       "Default ports are 7002 and 7003.");
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

    ImGui::Checkbox("Enable Robust Mode (MFSK)", &robust_mode_enabled_);
    ImGui::TextWrapped("Uses narrowband FSK for weak-signal hailing and low-speed data. "
                       "When combined with Gear Shift, hails on Robust then shifts to OFDM.");

    ImGui::Spacing();

    ImGui::Text("Bandwidth Mode:");
    ImGui::SameLine(180);
    ImGui::SetNextItemWidth(300);
    const char* bw_items[] = { "Auto (NB hail, WB upgrade)", "Narrowband Only (500 Hz)" };
    ImGui::Combo("##bw_mode", &bandwidth_mode_, bw_items, 2);
    ImGui::TextWrapped("Auto: all connections start narrowband. If both stations support wideband, "
                       "upgrades to 2344 Hz after connecting. NB Only: stays at 500 Hz always.");

    ImGui::Spacing();

    if (!gear_shift_enabled_) {
        ImGui::BeginDisabled();
    }

    ImGui::Text("Initial Configuration:");
    ImGui::SameLine(180);
    ImGui::SetNextItemWidth(300);
    const char* preview = config_to_string(initial_config_);
    if (ImGui::BeginCombo("##initial_config", preview)) {
        for (int i = 0; i < FULL_CONFIG_LADDER_SIZE; i++) {
            int cfg = FULL_CONFIG_LADDER[i];
            bool is_selected = (initial_config_ == cfg);
            if (ImGui::Selectable(config_to_string(cfg), is_selected))
                initial_config_ = cfg;
            if (is_selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    if (!gear_shift_enabled_) {
        ImGui::EndDisabled();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::TextWrapped("Gear shifting automatically adjusts the modulation configuration based on channel conditions. "
                       "Turboshift probes the link bidirectionally at connection time, then the success-based "
                       "ladder maintains the optimal rate during data transfer.");
}

void SetupDialog::renderAdvancedTab() {
    ImGui::Spacing();

    ImGui::Text("LDPC Decoder");
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::Text("Max Iterations:");
    ImGui::SameLine(180);
    ImGui::SetNextItemWidth(150);
    ImGui::SliderInt("##ldpc_iter", &ldpc_iterations_max_, 5, 50, "%d");

    ImGui::Spacing();

    ImGui::TextWrapped("Lower values reduce CPU load on slower hardware but may miss marginal frames. "
                       "Recommended: 50 for desktop, 15-25 for low-power devices. "
                       "Requires modem restart.");

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::Text("Frequency Sync");
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::Checkbox("Enable Coarse Frequency Search", &coarse_freq_sync_enabled_);

    ImGui::Spacing();

    ImGui::TextWrapped("Searches +/-30 Hz for crystal oscillator drift between HF radios. "
                       "Disable for loopback or same-clock setups to save CPU.");

    ImGui::Spacing();
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

void SetupDialog::renderSecurityTab() {
    ImGui::Spacing();

    ImGui::Text("Encryption");
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::Text("Encryption Mode:");
    ImGui::SameLine(180);
    ImGui::SetNextItemWidth(300);
    const char* enc_modes[] = {
        "Off (plaintext)",
        "Strict (hold data until PQ key exchange)",
        "Fast (classical-first, PQ upgrade later)"
    };
    ImGui::Combo("##enc_mode", &encryption_mode_, enc_modes, 3);

    ImGui::Spacing();

    if (encryption_mode_ == 0)
        ImGui::BeginDisabled();

    ImGui::TextWrapped(
        "Strict: X25519 + ML-KEM-768 hybrid key exchange completes before any data is sent. "
        "Provides post-quantum security from the first byte. "
        "Fast: X25519 key exchange enables ChaCha20-Poly1305 immediately, then "
        "upgrades to hybrid PQ in the background.");

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::Text("Pre-Shared Key (PSK):");
    ImGui::Spacing();
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##psk_hex", psk_hex_, sizeof(psk_hex_),
                     ImGuiInputTextFlags_CharsHexadecimal);

    ImGui::Spacing();
    ImGui::TextWrapped(
        "Optional authentication key (hex string, up to 64 bytes / 128 hex chars). "
        "Both stations must use the same PSK to connect. "
        "When set, prevents unauthorized stations from establishing encrypted sessions. "
        "Leave empty for unauthenticated encryption.");

    if (encryption_mode_ == 0)
        ImGui::EndDisabled();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Show current encryption status
    int active_mode = g_gui_state.encryption_mode.load();
    bool enc_active = g_gui_state.encryption_active.load();
    if (active_mode > 0 && enc_active) {
        ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.4f, 1.0f), "Encryption ACTIVE");
    } else if (active_mode > 0) {
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "Encryption enabled (waiting for connection)");
    } else {
        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "Encryption disabled");
    }

    ImGui::Spacing();
    ImGui::TextWrapped(
        "Note: Encryption settings take effect on the next connection. "
        "Both stations must have encryption enabled to establish an encrypted session. "
        "Requires modem restart to change mode.");
}
