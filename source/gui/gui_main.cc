/**
 * @file gui_main.cc
 * @brief Mercury HF Modem GUI - Main window and render loop
 *
 * Uses Dear ImGui with Win32 + OpenGL3 backend (no SDL2 dependency)
 */

#include "gui/gui_state.h"
#include "gui/gui_main.h"
#include "gui/ini_parser.h"
#include "gui/widgets/waterfall.h"
#include "gui/dialogs/soundcard_dialog.h"
#include "gui/dialogs/setup_dialog.h"
#include "common/common_defines.h"

// ImGui headers
#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_win32.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <GL/gl.h>
#include <cstdio>
#include <cmath>

// Global GUI state - use Meyer's Singleton to avoid static init order fiasco
// The macro g_gui_state in gui_state.h calls this function
st_gui_state& get_gui_state() {
    static st_gui_state instance;
    return instance;
}

// Window data
static HGLRC g_hRC = nullptr;
static HDC g_hDC = nullptr;
static HWND g_hWnd = nullptr;
static int g_Width = 800;
static int g_Height = 600;

// Forward declarations
static bool CreateDeviceWGL(HWND hWnd);
static void CleanupDeviceWGL();
static LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// ========== Custom Widgets ==========

// State for slider auto-save
static float s_last_tx_gain = 0.0f;
static float s_last_rx_gain = 0.0f;
static bool s_tx_slider_active = false;
static bool s_rx_slider_active = false;

// State for auto-tune control loop
static DWORD s_autotune_last_adjust_time = 0;
static float s_autotune_last_signal = -100.0f;
static int s_autotune_stable_frames = 0;
static bool s_autotune_waiting = false;

// Smart auto-tune: measure gain-to-signal relationship
static int s_autotune_phase = 0;           // 0=probe, 1=measure, 2=jump, 3=fine-tune
static float s_autotune_probe_gain = 0.0f; // Gain before probe
static float s_autotune_probe_signal = 0.0f; // Signal before probe
static float s_autotune_gain_factor = 1.0f;  // dB signal change per dB gain change

/**
 * @brief Draw a tuning meter with zones (too quiet / just right / too loud)
 * Targets 0 dBm signal strength as the "just right" zone
 */
static void DrawTuningMeter(float signal_dbm) {
    ImGui::PushID("TuningMeter");

    ImGui::Text("RX Level Tuning");
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Calibrate with NO signal present.\n"
                          "Adjust RX Gain until noise floor reads ~0 dBm.\n"
                          "This ensures proper signal level interpretation.");
    }

    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImVec2 size(200, 25);
    ImDrawList* draw_list = ImGui::GetWindowDrawList();

    // Define zones: Too Quiet (<-10), Just Right (-10 to +10), Too Loud (>+10)
    const float quiet_threshold = -10.0f;
    const float loud_threshold = 10.0f;
    const float min_display = -30.0f;
    const float max_display = 30.0f;

    // Draw zone backgrounds
    float zone_width = size.x / 3.0f;

    // Too Quiet zone (left third) - orange/red
    draw_list->AddRectFilled(pos, ImVec2(pos.x + zone_width, pos.y + size.y),
                             IM_COL32(100, 50, 0, 255));

    // Just Right zone (middle third) - green
    draw_list->AddRectFilled(ImVec2(pos.x + zone_width, pos.y),
                             ImVec2(pos.x + zone_width * 2, pos.y + size.y),
                             IM_COL32(0, 80, 0, 255));

    // Too Loud zone (right third) - red
    draw_list->AddRectFilled(ImVec2(pos.x + zone_width * 2, pos.y),
                             ImVec2(pos.x + size.x, pos.y + size.y),
                             IM_COL32(100, 0, 0, 255));

    // Draw zone labels
    ImVec2 text_size;
    float label_y = pos.y + 3;

    draw_list->AddText(ImVec2(pos.x + 5, label_y), IM_COL32(200, 150, 100, 200), "Quiet");
    draw_list->AddText(ImVec2(pos.x + zone_width + 15, label_y), IM_COL32(100, 200, 100, 200), "Good");
    draw_list->AddText(ImVec2(pos.x + zone_width * 2 + 10, label_y), IM_COL32(200, 100, 100, 200), "Loud");

    // Calculate indicator position
    float normalized = (signal_dbm - min_display) / (max_display - min_display);
    if (normalized < 0.0f) normalized = 0.0f;
    if (normalized > 1.0f) normalized = 1.0f;

    float indicator_x = pos.x + normalized * size.x;

    // Draw indicator triangle (pointing down from top)
    ImU32 indicator_color;
    if (signal_dbm < quiet_threshold) {
        indicator_color = IM_COL32(255, 150, 50, 255);  // Orange
    } else if (signal_dbm > loud_threshold) {
        indicator_color = IM_COL32(255, 50, 50, 255);   // Red
    } else {
        indicator_color = IM_COL32(50, 255, 50, 255);   // Green
    }

    draw_list->AddTriangleFilled(
        ImVec2(indicator_x, pos.y + 2),
        ImVec2(indicator_x - 6, pos.y + size.y - 2),
        ImVec2(indicator_x + 6, pos.y + size.y - 2),
        indicator_color);

    // Draw border
    draw_list->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y),
                       IM_COL32(128, 128, 128, 255));

    ImGui::Dummy(size);

    // Show status text
    ImGui::SameLine();
    if (signal_dbm < quiet_threshold) {
        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "%.1f dBm - Increase RX Gain", signal_dbm);
    } else if (signal_dbm > loud_threshold) {
        ImGui::TextColored(ImVec4(1.0f, 0.2f, 0.2f, 1.0f), "%.1f dBm - Decrease RX Gain", signal_dbm);
    } else {
        ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.2f, 1.0f), "%.1f dBm - Good!", signal_dbm);
    }

    ImGui::PopID();
}

/**
 * @brief Draw a horizontal meter (VU, SNR, Signal strength style)
 * Shows colored zone backgrounds with a bright bar indicator showing current value
 * @param tooltip If non-null, adds a (?) help icon with the given tooltip text
 * @param invert_colors If true, swap red/green (for meters where higher is better)
 */
static void DrawMeter(const char* label, float value, float min_val, float max_val,
                      float yellow_threshold, float red_threshold,
                      const char* format = "%.1f dB", const char* tooltip = nullptr,
                      bool invert_colors = false) {
    ImGui::PushID(label);

    // Calculate normalized position
    float normalized = (value - min_val) / (max_val - min_val);
    if (normalized < 0.0f) normalized = 0.0f;
    if (normalized > 1.0f) normalized = 1.0f;

    float yellow_norm = (yellow_threshold - min_val) / (max_val - min_val);
    float red_norm = (red_threshold - min_val) / (max_val - min_val);

    ImGui::Text("%s", label);
    if (tooltip) {
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", tooltip);
        }
    }

    // Draw background
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImVec2 size(180, 20);  // Slightly narrower to leave room for value text
    ImDrawList* draw_list = ImGui::GetWindowDrawList();

    // Colors - swap red/green if inverted (for meters where higher is better)
    ImU32 left_dim = invert_colors ? IM_COL32(60, 0, 0, 255) : IM_COL32(0, 60, 0, 255);
    ImU32 mid_dim = IM_COL32(60, 60, 0, 255);
    ImU32 right_dim = invert_colors ? IM_COL32(0, 60, 0, 255) : IM_COL32(60, 0, 0, 255);
    ImU32 left_bright = invert_colors ? IM_COL32(220, 0, 0, 255) : IM_COL32(0, 220, 0, 255);
    ImU32 mid_bright = IM_COL32(220, 220, 0, 255);
    ImU32 right_bright = invert_colors ? IM_COL32(0, 220, 0, 255) : IM_COL32(220, 0, 0, 255);

    // Draw colored zone backgrounds (dim)
    float zone1_end = pos.x + yellow_norm * size.x;
    float zone2_end = pos.x + red_norm * size.x;

    // Left zone background
    draw_list->AddRectFilled(pos, ImVec2(zone1_end, pos.y + size.y), left_dim);
    // Middle zone background
    draw_list->AddRectFilled(ImVec2(zone1_end, pos.y), ImVec2(zone2_end, pos.y + size.y), mid_dim);
    // Right zone background
    draw_list->AddRectFilled(ImVec2(zone2_end, pos.y), ImVec2(pos.x + size.x, pos.y + size.y), right_dim);

    // Draw bright bar up to current value
    float bar_end = pos.x + normalized * size.x;

    // Left segment (bright)
    if (bar_end > pos.x) {
        float seg_end = (bar_end < zone1_end) ? bar_end : zone1_end;
        if (seg_end > pos.x) {
            draw_list->AddRectFilled(pos, ImVec2(seg_end, pos.y + size.y), left_bright);
        }
    }

    // Middle segment (bright)
    if (bar_end > zone1_end) {
        float seg_end = (bar_end < zone2_end) ? bar_end : zone2_end;
        if (seg_end > zone1_end) {
            draw_list->AddRectFilled(ImVec2(zone1_end, pos.y), ImVec2(seg_end, pos.y + size.y), mid_bright);
        }
    }

    // Right segment (bright)
    if (bar_end > zone2_end) {
        draw_list->AddRectFilled(ImVec2(zone2_end, pos.y), ImVec2(bar_end, pos.y + size.y), right_bright);
    }

    // Border
    draw_list->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y),
                       IM_COL32(128, 128, 128, 255));

    // Advance cursor
    ImGui::Dummy(size);

    // Value text
    ImGui::SameLine();
    ImGui::Text(format, value);

    ImGui::PopID();
}

/**
 * @brief Draw status LED indicator
 */
static void DrawLED(const char* label, bool active, ImU32 active_color = IM_COL32(0, 255, 0, 255)) {
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImDrawList* draw_list = ImGui::GetWindowDrawList();

    float radius = 8.0f;
    ImVec2 center(pos.x + radius, pos.y + radius);

    // Draw LED
    if (active) {
        draw_list->AddCircleFilled(center, radius, active_color);
        // Glow effect
        draw_list->AddCircle(center, radius + 2, active_color, 0, 2.0f);
    } else {
        draw_list->AddCircleFilled(center, radius, IM_COL32(60, 60, 60, 255));
    }
    draw_list->AddCircle(center, radius, IM_COL32(128, 128, 128, 255));

    ImGui::Dummy(ImVec2(radius * 2, radius * 2));
    ImGui::SameLine();
    ImGui::Text("%s", label);
}

/**
 * @brief Get link status string
 */
static const char* GetLinkStatusString(int status) {
    switch (status) {
        case 0: return "IDLE";
        case 1: return "CONNECTING";
        case 2: return "CONNECTED";
        case 3: return "DISCONNECTING";
        case 4: return "LISTENING";
        case 5: return "CONN RECEIVED";
        case 6: return "CONN ACCEPTED";
        case 7: return "NEGOTIATING";
        default: return "UNKNOWN";
    }
}

/**
 * @brief Draw constellation (IQ scatter) diagram
 */
static void DrawConstellation(float width, float height) {
    ImGui::PushID("Constellation");

    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImDrawList* draw_list = ImGui::GetWindowDrawList();

    bool is_mfsk = g_gui_state.constellation_is_mfsk.load();

    // Dark background
    draw_list->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + height),
                             IM_COL32(15, 15, 20, 255));

    if (is_mfsk) {
        // MFSK mode - no constellation, show text
        const char* mode_text = (width >= 120) ? "MFSK (non-coherent)" : "MFSK";
        ImVec2 text_size = ImGui::CalcTextSize(mode_text);
        draw_list->AddText(
            ImVec2(pos.x + (width - text_size.x) / 2, pos.y + height / 2 - 10),
            IM_COL32(150, 150, 150, 255), mode_text);
        const char* no_iq = "No IQ data";
        ImVec2 no_iq_size = ImGui::CalcTextSize(no_iq);
        draw_list->AddText(
            ImVec2(pos.x + (width - no_iq_size.x) / 2, pos.y + height / 2 + 8),
            IM_COL32(100, 100, 100, 200), no_iq);
    } else {
        // Draw crosshairs
        float cx = pos.x + width / 2;
        float cy = pos.y + height / 2;
        draw_list->AddLine(ImVec2(pos.x, cy), ImVec2(pos.x + width, cy),
                           IM_COL32(50, 50, 50, 255));
        draw_list->AddLine(ImVec2(cx, pos.y), ImVec2(cx, pos.y + height),
                           IM_COL32(50, 50, 50, 255));

        // Scale: normalized PSK/QAM values are roughly in [-2, 2]
        float dim = (width < height) ? width : height;
        float scale = dim / 4.5f;

        // Read constellation data under lock
        float local_i[GUI_CONSTELLATION_MAX_POINTS];
        float local_q[GUI_CONSTELLATION_MAX_POINTS];
        int count;
        {
            GuiLockGuard lock(g_gui_state.constellation_mutex);
            count = g_gui_state.constellation_count;
            if (count > GUI_CONSTELLATION_MAX_POINTS) count = GUI_CONSTELLATION_MAX_POINTS;
            memcpy(local_i, g_gui_state.constellation_i, count * sizeof(float));
            memcpy(local_q, g_gui_state.constellation_q, count * sizeof(float));
        }

        // Draw scatter points
        for (int i = 0; i < count; i++) {
            float px = cx + local_i[i] * scale;
            float py = cy - local_q[i] * scale;  // Negate Q for screen coords
            if (px >= pos.x && px <= pos.x + width &&
                py >= pos.y && py <= pos.y + height) {
                draw_list->AddCircleFilled(ImVec2(px, py), 1.5f,
                                           IM_COL32(0, 200, 255, 180));
            }
        }

        // Show placeholder when no data
        if (count == 0) {
            const char* waiting = (width >= 120) ? "Waiting for data..." : "No data";
            ImVec2 ws = ImGui::CalcTextSize(waiting);
            float tx = pos.x + (width - ws.x) / 2;
            if (tx < pos.x + 2) tx = pos.x + 2;  // Clamp to left edge
            draw_list->AddText(ImVec2(tx, pos.y + height / 2 - 6),
                IM_COL32(80, 80, 80, 200), waiting);
        }
    }

    // Border
    draw_list->AddRect(pos, ImVec2(pos.x + width, pos.y + height),
                       IM_COL32(80, 80, 80, 255));

    ImGui::Dummy(ImVec2(width, height));
    ImGui::PopID();
}

// ========== Main GUI Rendering ==========

static void RenderGUI() {
    ImGuiIO& io = ImGui::GetIO();

    // Main menu bar
    static bool show_waterfall = true;
    static bool show_about = false;

    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("Settings")) {
            if (ImGui::MenuItem("Sound Card...")) {
                g_soundcard_dialog.open();
            }
            if (ImGui::MenuItem("Modem Setup...")) {
                g_setup_dialog.open();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Save Settings")) {
                std::string path = getDefaultConfigPath();
                g_settings.save(path);
            }
            if (ImGui::MenuItem("Load Settings")) {
                std::string path = getDefaultConfigPath();
                if (g_settings.load(path)) {
                    // Update GUI state from loaded settings
                    g_gui_state.tx_gain_db.store(g_settings.tx_gain_db);
                    g_gui_state.rx_gain_db.store(g_settings.rx_gain_db);
                }
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View")) {
            if (ImGui::MenuItem("Waterfall", nullptr, &show_waterfall)) {
                // When toggled, enable/disable FFT computation
                g_waterfall.setEnabled(show_waterfall);
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Help")) {
            if (ImGui::MenuItem("About Mercury...")) {
                show_about = true;
            }
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }

    // Render dialogs
    if (g_soundcard_dialog.isOpen()) {
        if (g_soundcard_dialog.render()) {
            // Settings were applied - could trigger audio restart here
        }
    }

    if (g_setup_dialog.isOpen()) {
        if (g_setup_dialog.render()) {
            // Settings were applied
        }
    }

    // About dialog
    if (show_about) {
        ImGui::OpenPopup("About Mercury");
        show_about = false;
    }
    if (ImGui::BeginPopupModal("About Mercury", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Mercury HF Modem");
        ImGui::Separator();
        ImGui::Text("An open-source HF data modem");
        ImGui::Text("Compatible with VARA protocol");
        ImGui::Spacing();
        ImGui::Text("https://github.com/Rhizomatica/mercury");
        ImGui::Spacing();
        if (ImGui::Button("OK", ImVec2(120, 0))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // Main window (fills the screen below menu bar)
    ImGui::SetNextWindowPos(ImVec2(0, ImGui::GetFrameHeight()));
    ImGui::SetNextWindowSize(ImVec2((float)g_Width, (float)g_Height - ImGui::GetFrameHeight() - 25));
    ImGui::Begin("Mercury HF Modem", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                 ImGuiWindowFlags_NoBringToFrontOnFocus);

    // ========== Top Row: Meters ==========
    ImGui::BeginChild("Meters", ImVec2(0, 150), true);

    ImGui::Columns(3, "meter_columns", false);

    // Rate-limited meter values (~15 Hz update instead of 60 fps)
    // This reduces CPU load significantly on weaker systems
    static DWORD last_meter_update = 0;
    static float cached_vu_db = -60.0f;
    static float cached_signal = 0.0f;
    static float cached_snr_rx = -99.9f;
    static float cached_snr_tx = -99.9f;
    static float cached_load = 0.0f;
    static float cached_buf_fill = 0.0f;

    DWORD now = GetTickCount();
    if (now - last_meter_update >= 66) {  // ~15 Hz
        last_meter_update = now;
        cached_vu_db = (float)g_gui_state.vu_rms_db;
        cached_signal = (float)g_gui_state.signal_strength_dbm.load();
        cached_snr_rx = (float)g_gui_state.snr_uplink_db.load();
        cached_snr_tx = (float)g_gui_state.snr_downlink_db.load();
        cached_load = g_gui_state.processing_load.load() * 100.0f;  // as percentage
        cached_buf_fill = g_gui_state.buffer_fill_pct.load();
    }

    // VU Meter and Signal Strength
    {
        DrawMeter("Audio Input", cached_vu_db, -60.0f, 0.0f, -12.0f, -3.0f);

        DrawMeter("Signal Strength", cached_signal, -10.0f, 50.0f, -4.0f, 10.0f, "%.1f dBm",
                  "Received signal power level.\n"
                  "Measured locally from incoming audio.\n"
                  "Use RX Gain to calibrate noise floor to ~0 dBm.",
                  true);  // invert colors: higher is better
    }

    ImGui::NextColumn();

    // SNR Meters (Uplink and Downlink)
    {
        // Incoming SNR - what WE measure of their transmissions
        DrawMeter("RX SNR (Incoming)", cached_snr_rx, -10.0f, 50.0f, 10.0f, 25.0f, "%.1f dB",
                  "SNR of signals we RECEIVE from the remote station.\n"
                  "Measured locally by our receiver.\n"
                  "Higher is better (-10 to +50 dB range).",
                  true);  // invert colors: higher is better

        ImGui::Spacing();

        // Outgoing SNR - what THEY measure of our transmissions
        DrawMeter("TX SNR (Outgoing)", cached_snr_tx, -10.0f, 50.0f, 10.0f, 25.0f, "%.1f dB",
                  "SNR of signals the remote station RECEIVES from us.\n"
                  "Reported back by the remote station.\n"
                  "Higher is better (-10 to +50 dB range).",
                  true);  // invert colors: higher is better
    }

    ImGui::NextColumn();

    // Performance Meters
    {
        DrawMeter("Decode Time", cached_load, 0.0f, 200.0f, 80.0f, 120.0f, "%.2f%%",
                  "Decode time as %% of real-time frame period.\n"
                  "Below 100%% = keeping up with incoming audio.\n"
                  "Above 100%% = falling behind, frames will be lost.\n"
                  "Note: actual CPU use is lower (single-thread metric).");

        ImGui::Spacing();

        DrawMeter("RX Buffer", cached_buf_fill, 0.0f, 100.0f, 60.0f, 85.0f, "%.2f%%",
                  "Audio capture ring buffer fill level.\n"
                  "High values indicate processing can't keep up.\n"
                  "If it hits 100%, audio samples are lost.");
    }

    ImGui::Columns(1);
    ImGui::EndChild();

    // ========== Signal Quality: Constellation + Status ==========
    ImGui::BeginChild("SignalQuality", ImVec2(0, 140), true);
    {
        ImVec2 avail_sq = ImGui::GetContentRegionAvail();
        float diagram_size = avail_sq.y;
        if (diagram_size < 60) diagram_size = 60;

        // Left: Constellation diagram (square, full height)
        DrawConstellation(diagram_size, diagram_size);

        ImGui::SameLine();

        // Right: Status and throughput
        ImGui::BeginGroup();
        {
            int cur_config = g_gui_state.current_configuration.load();
            double throughput = g_gui_state.throughput_bps.load();
            double phy_rate = g_gui_state.current_bitrate.load();
            long long bytes_tx = g_gui_state.bytes_acked_total.load();
            long long bytes_rx = g_gui_state.bytes_received_total.load();
            int link_status = g_gui_state.link_status.load();
            float snr_rx = (float)g_gui_state.snr_uplink_db.load();
            float freq_off = (float)g_gui_state.freq_offset_hz.load();

            // Config name (prominent)
            ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "%s",
                               config_to_short_string(cur_config));

            // Link status
            const char* link_str = GetLinkStatusString(link_status);
            if (link_status == 2)
                ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f), "%s", link_str);
            else
                ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "%s", link_str);

            // SNR and freq offset
            if (snr_rx > -90.0f)
                ImGui::Text("SNR %.1f dB  |  AFC %.1f Hz", snr_rx, freq_off);
            else
                ImGui::Text("SNR ---  |  AFC %.1f Hz", freq_off);

            ImGui::Spacing();

            // Throughput
            if (throughput >= 1000.0)
                ImGui::Text("%.1f kbps", throughput / 1000.0);
            else
                ImGui::Text("%.0f bps", throughput);

            // PHY rate + efficiency
            if (phy_rate > 0 && throughput > 0)
                ImGui::Text("PHY %.0f bps (%.0f%%)", phy_rate, 100.0 * throughput / phy_rate);
            else if (phy_rate > 0)
                ImGui::Text("PHY %.0f bps", phy_rate);

            // Bytes transferred
            long long total_bytes = bytes_tx + bytes_rx;
            if (total_bytes >= 1048576)
                ImGui::Text("%.1f MB transferred", total_bytes / 1048576.0);
            else if (total_bytes >= 1024)
                ImGui::Text("%.1f KB transferred", total_bytes / 1024.0);
            else if (total_bytes > 0)
                ImGui::Text("%lld B transferred", total_bytes);
        }
        ImGui::EndGroup();
    }
    ImGui::EndChild();

    // ========== Controls: Gain Levels ==========
    ImGui::BeginChild("Controls", ImVec2(0, 190), true);

    // Lock checkbox at top
    bool gains_locked = g_gui_state.gains_locked.load();
    if (ImGui::Checkbox("Lock Gain Adjustments", &gains_locked)) {
        g_gui_state.gains_locked.store(gains_locked);
        // Save immediately when lock state changes
        g_settings.gains_locked = gains_locked;
        std::string path = getDefaultConfigPath();
        g_settings.save(path);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Unlock to adjust gain sliders.\nSettings auto-save when you stop adjusting.");
    }

    ImGui::Separator();
    ImGui::Spacing();

    ImGui::Columns(3, "control_columns", false);

    // TX Gain / Drive Level
    {
        ImGui::Text("TX Drive Level");
        float tx_gain = (float)g_gui_state.tx_gain_db.load();

        // Disable slider if locked
        if (gains_locked) {
            ImGui::BeginDisabled();
        }

        bool tx_changed = ImGui::SliderFloat("##tx_gain", &tx_gain, -20.0f, 6.0f, "%.1f dB");
        bool tx_active = ImGui::IsItemActive();

        if (gains_locked) {
            ImGui::EndDisabled();
        }

        if (tx_changed && !gains_locked) {
            g_gui_state.tx_gain_db.store(tx_gain);
        }

        // Auto-save when slider is released
        if (s_tx_slider_active && !tx_active && !gains_locked) {
            // Slider was just released - save settings
            g_settings.tx_gain_db = tx_gain;
            std::string path = getDefaultConfigPath();
            g_settings.save(path);
            s_last_tx_gain = tx_gain;
        }
        s_tx_slider_active = tx_active;

        // Tune button
        bool tune_active = g_gui_state.tune_active.load();
        if (tune_active) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.2f, 0.2f, 1.0f));
            g_gui_state.is_transmitting.store(true);
        }
        if (ImGui::Button(tune_active ? "TUNE (ON)" : "TUNE", ImVec2(100, 30))) {
            bool new_state = !tune_active;
            g_gui_state.tune_active.store(new_state);
            printf("TUNE: %s - 1500 Hz tone to audio output\n", new_state ? "ON" : "OFF");
            if (!new_state) {
                g_gui_state.is_transmitting.store(false);
            }
        }
        if (tune_active) {
            ImGui::PopStyleColor();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("1500 Hz");
    }

    ImGui::NextColumn();

    // RX Gain with Auto-Tune
    {
        ImGui::Text("RX Gain");
        float rx_gain = (float)g_gui_state.rx_gain_db.load();
        float signal_dbm = (float)g_gui_state.signal_strength_dbm.load();

        // Disable slider if locked
        if (gains_locked) {
            ImGui::BeginDisabled();
        }

        bool rx_changed = ImGui::SliderFloat("##rx_gain", &rx_gain, -15.0f, 35.0f, "%.1f dB");
        bool rx_active = ImGui::IsItemActive();

        if (gains_locked) {
            ImGui::EndDisabled();
        }

        if (rx_changed && !gains_locked) {
            g_gui_state.rx_gain_db.store(rx_gain);
        }

        // Auto-save when slider is released
        if (s_rx_slider_active && !rx_active && !gains_locked) {
            // Slider was just released - save settings
            g_settings.rx_gain_db = rx_gain;
            std::string path = getDefaultConfigPath();
            g_settings.save(path);
            s_last_rx_gain = rx_gain;
        }
        s_rx_slider_active = rx_active;

        // Auto-tune button
        bool auto_tune_active = g_gui_state.auto_tune_active.load();
        if (auto_tune_active) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.6f, 0.2f, 1.0f));
        }

        if (gains_locked) {
            ImGui::BeginDisabled();
        }

        if (ImGui::Button(auto_tune_active ? "AUTO-TUNE..." : "Auto-Tune", ImVec2(100, 25))) {
            if (!auto_tune_active) {
                // Starting auto-tune - reset state
                g_gui_state.auto_tune_active.store(true);
                s_autotune_last_adjust_time = GetTickCount();
                s_autotune_last_signal = signal_dbm;
                s_autotune_stable_frames = 0;
                s_autotune_waiting = true;
                s_autotune_phase = 0;  // Start with probe phase
                s_autotune_probe_gain = rx_gain;
                s_autotune_probe_signal = signal_dbm;
                s_autotune_gain_factor = 1.0f;
                printf("Auto-tune started: Phase 0 (probe) - measuring system response...\n");
            } else {
                g_gui_state.auto_tune_active.store(false);
                printf("Auto-tune cancelled\n");
            }
        }

        if (gains_locked) {
            ImGui::EndDisabled();
        }

        if (auto_tune_active) {
            ImGui::PopStyleColor();

            // Smart auto-tune with gain-to-signal relationship measurement
            const float target_dbm = 0.0f;
            const float fine_tolerance = 2.0f;     // Done when within +/-2 dB
            const float probe_step = 5.0f;         // Probe with 5dB change
            const DWORD settle_time_ms = 4000;     // Wait 4 seconds for settling
            const float signal_stable_threshold = 1.0f;

            DWORD now = GetTickCount();
            DWORD elapsed = now - s_autotune_last_adjust_time;

            if (signal_dbm > -90.0f) {
                float error = target_dbm - signal_dbm;
                float abs_error = fabs(error);

                // Check signal stability
                float signal_change = fabs(signal_dbm - s_autotune_last_signal);
                if (signal_change < signal_stable_threshold) {
                    s_autotune_stable_frames++;
                } else {
                    s_autotune_stable_frames = 0;
                }
                s_autotune_last_signal = signal_dbm;

                // Show phase and countdown
                const char* phase_names[] = {"Probe", "Measure", "Jump", "Fine"};
                if (elapsed < settle_time_ms) {
                    ImGui::SameLine();
                    ImGui::Text("%s %.1fs", phase_names[s_autotune_phase], (settle_time_ms - elapsed) / 1000.0f);
                } else {
                    ImGui::SameLine();
                    ImGui::Text("%s...", phase_names[s_autotune_phase]);
                }

                // State machine for smart auto-tune
                bool stable = (elapsed >= settle_time_ms && s_autotune_stable_frames >= 30);

                switch (s_autotune_phase) {
                case 0:  // PROBE: Record initial state, apply probe step
                    if (stable) {
                        s_autotune_probe_gain = rx_gain;
                        s_autotune_probe_signal = signal_dbm;

                        // Apply probe step (go up if too quiet, down if too loud)
                        float probe_direction = (error > 0) ? 1.0f : -1.0f;
                        rx_gain += probe_direction * probe_step;
                        if (rx_gain < -15.0f) rx_gain = -15.0f;
                        if (rx_gain > 35.0f) rx_gain = 35.0f;

                        g_gui_state.rx_gain_db.store(rx_gain);
                        s_autotune_last_adjust_time = now;
                        s_autotune_stable_frames = 0;
                        s_autotune_phase = 1;
                        printf("Auto-tune Phase 1: probed with %.1f dB step (gain: %.1f -> %.1f)\n",
                               probe_direction * probe_step, s_autotune_probe_gain, rx_gain);
                    }
                    break;

                case 1:  // MEASURE: Measure response, calculate gain factor
                    if (stable) {
                        float gain_change = rx_gain - s_autotune_probe_gain;
                        float signal_change_measured = signal_dbm - s_autotune_probe_signal;

                        if (fabs(gain_change) > 0.1f && fabs(signal_change_measured) > 0.1f) {
                            // Calculate gain factor: how much signal changes per dB of gain
                            s_autotune_gain_factor = signal_change_measured / gain_change;
                            printf("Auto-tune: measured gain factor = %.2f dB/dB (gain %.1f->%.1f, signal %.1f->%.1f)\n",
                                   s_autotune_gain_factor, s_autotune_probe_gain, rx_gain,
                                   s_autotune_probe_signal, signal_dbm);
                        } else {
                            // Couldn't measure - use default 1:1
                            s_autotune_gain_factor = 1.0f;
                            printf("Auto-tune: couldn't measure response, using 1:1 ratio\n");
                        }

                        // Clamp gain factor to reasonable range (0.3 to 3.0)
                        if (s_autotune_gain_factor < 0.3f) s_autotune_gain_factor = 0.3f;
                        if (s_autotune_gain_factor > 3.0f) s_autotune_gain_factor = 3.0f;

                        // Now calculate predicted optimal gain
                        error = target_dbm - signal_dbm;  // Current error
                        float predicted_gain_change = error / s_autotune_gain_factor;
                        float predicted_gain = rx_gain + predicted_gain_change;

                        // Clamp to valid range
                        if (predicted_gain < -15.0f) predicted_gain = -15.0f;
                        if (predicted_gain > 35.0f) predicted_gain = 35.0f;

                        rx_gain = predicted_gain;
                        g_gui_state.rx_gain_db.store(rx_gain);
                        s_autotune_last_adjust_time = now;
                        s_autotune_stable_frames = 0;
                        s_autotune_phase = 2;
                        printf("Auto-tune Phase 2: jumping to predicted gain %.1f dB (change: %.1f)\n",
                               rx_gain, predicted_gain_change);
                    }
                    break;

                case 2:  // JUMP: Check if prediction was good, go to fine-tune if needed
                    if (stable) {
                        if (abs_error <= fine_tolerance) {
                            // Prediction was good - done!
                            g_gui_state.auto_tune_active.store(false);
                            g_settings.rx_gain_db = rx_gain;
                            std::string path = getDefaultConfigPath();
                            g_settings.save(path);
                            printf("Auto-tune complete: RX Gain = %.1f dB, Signal = %.1f dBm (prediction accurate!)\n",
                                   rx_gain, signal_dbm);
                        } else {
                            // Need fine-tuning
                            s_autotune_phase = 3;
                            printf("Auto-tune Phase 3: fine-tuning (error: %.1f dBm)\n", error);
                        }
                    }
                    break;

                case 3:  // FINE-TUNE: Small adjustments using measured gain factor
                    if (stable) {
                        if (abs_error <= fine_tolerance) {
                            // Done!
                            g_gui_state.auto_tune_active.store(false);
                            g_settings.rx_gain_db = rx_gain;
                            std::string path = getDefaultConfigPath();
                            g_settings.save(path);
                            printf("Auto-tune complete: RX Gain = %.1f dB, Signal = %.1f dBm\n",
                                   rx_gain, signal_dbm);
                        } else {
                            // Use measured gain factor for fine adjustment
                            float adjustment = error / s_autotune_gain_factor;
                            // Limit fine adjustment to +/-3 dB max
                            if (adjustment > 3.0f) adjustment = 3.0f;
                            if (adjustment < -3.0f) adjustment = -3.0f;

                            rx_gain += adjustment;
                            if (rx_gain < -15.0f) rx_gain = -15.0f;
                            if (rx_gain > 35.0f) rx_gain = 35.0f;

                            g_gui_state.rx_gain_db.store(rx_gain);
                            s_autotune_last_adjust_time = now;
                            s_autotune_stable_frames = 0;
                            printf("Auto-tune: fine-tune adjustment %.1f dB -> gain %.1f dB\n",
                                   adjustment, rx_gain);
                        }
                    }
                    break;
                }
            }
        }

        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Smart auto-tune:\n1. Probe: measures system response\n2. Jump: predicts optimal gain\n3. Fine-tune: adjusts if needed\nClick again to stop.");
        }

        // Tuning meter - only show when gains are unlocked
        ImGui::Spacing();
        if (!gains_locked) {
            DrawTuningMeter(signal_dbm);
        } else {
            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "Unlock gains to tune RX level");
        }
    }

    ImGui::NextColumn();

    // AFC Display
    {
        ImGui::Text("AFC / Freq Offset");
        float freq_offset = (float)g_gui_state.freq_offset_hz.load();

        // Draw centered indicator
        ImVec2 pos = ImGui::GetCursorScreenPos();
        ImVec2 size(150, 25);
        ImDrawList* draw_list = ImGui::GetWindowDrawList();

        // Background
        draw_list->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y),
                                 IM_COL32(40, 40, 40, 255));

        // Center line
        float center_x = pos.x + size.x / 2;
        draw_list->AddLine(ImVec2(center_x, pos.y), ImVec2(center_x, pos.y + size.y),
                           IM_COL32(100, 100, 100, 255));

        // Offset indicator (+/-100 Hz range)
        float offset_norm = freq_offset / 100.0f;
        if (offset_norm < -1.0f) offset_norm = -1.0f;
        if (offset_norm > 1.0f) offset_norm = 1.0f;
        float indicator_x = center_x + offset_norm * (size.x / 2 - 5);
        draw_list->AddTriangleFilled(
            ImVec2(indicator_x, pos.y + 5),
            ImVec2(indicator_x - 5, pos.y + size.y - 5),
            ImVec2(indicator_x + 5, pos.y + size.y - 5),
            IM_COL32(0, 200, 255, 255));

        draw_list->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y),
                           IM_COL32(128, 128, 128, 255));

        ImGui::Dummy(size);
        ImGui::Text("%.1f Hz", freq_offset);
    }

    ImGui::Columns(1);
    ImGui::EndChild();

    // ========== Waterfall Display ==========
    if (show_waterfall) {
        ImGui::BeginChild("Waterfall", ImVec2(0, 120), true);
        ImGui::Text("Waterfall");
        ImGui::SameLine(ImGui::GetWindowWidth() - 100);
        ImGui::Text("-100 to 0 dB");

        ImVec2 avail = ImGui::GetContentRegionAvail();
        g_waterfall.render(avail.x, avail.y - 25);

        ImGui::EndChild();
    }

    ImGui::End();

    // ========== Status Bar ==========
    ImGui::SetNextWindowPos(ImVec2(0, (float)g_Height - 25));
    ImGui::SetNextWindowSize(ImVec2((float)g_Width, 25));
    ImGui::Begin("StatusBar", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                 ImGuiWindowFlags_NoCollapse);

    int link_status = g_gui_state.link_status.load();
    const char* status_str = GetLinkStatusString(link_status);

    // Status LED
    bool connected = (link_status == 2);
    bool listening = (link_status == 4);

    ImVec2 led_pos = ImGui::GetCursorScreenPos();
    ImDrawList* draw_list2 = ImGui::GetWindowDrawList();
    float led_radius = 6.0f;
    ImVec2 led_center(led_pos.x + led_radius + 2, led_pos.y + 10);

    ImU32 led_color = IM_COL32(60, 60, 60, 255);
    if (connected) led_color = IM_COL32(0, 255, 0, 255);
    else if (listening) led_color = IM_COL32(255, 200, 0, 255);

    draw_list2->AddCircleFilled(led_center, led_radius, led_color);
    draw_list2->AddCircle(led_center, led_radius, IM_COL32(128, 128, 128, 255));

    ImGui::Dummy(ImVec2(led_radius * 2 + 8, 0));
    ImGui::SameLine();

    ImGui::Text("%s", status_str);
    ImGui::SameLine(150);
    ImGui::Text("| %s |", config_to_short_string(g_gui_state.current_configuration.load()));
    ImGui::SameLine();

    bool is_tx = g_gui_state.is_transmitting.load();
    if (is_tx) {
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "TX");
    } else {
        ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f), "RX");
    }

    ImGui::SameLine(350);
    ImGui::Text("| %.1f bps |", g_gui_state.current_bitrate.load());

    // Status LEDs in status bar
    ImGui::SameLine();
    {
        bool sb_tx = g_gui_state.is_transmitting.load();
        bool sb_rx = g_gui_state.is_receiving.load();
        bool sb_data = g_gui_state.data_activity.load();
        bool sb_ack = g_gui_state.ack_activity.load();

        if (sb_tx)   ImGui::TextColored(ImVec4(1.0f, 0.2f, 0.2f, 1.0f), "TX");
        else         ImGui::TextColored(ImVec4(0.3f, 0.3f, 0.3f, 1.0f), "TX");
        ImGui::SameLine();
        if (sb_rx)   ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.2f, 1.0f), "RX");
        else         ImGui::TextColored(ImVec4(0.3f, 0.3f, 0.3f, 1.0f), "RX");
        ImGui::SameLine();
        if (sb_data) ImGui::TextColored(ImVec4(0.0f, 0.8f, 1.0f, 1.0f), "DATA");
        else         ImGui::TextColored(ImVec4(0.3f, 0.3f, 0.3f, 1.0f), "DATA");
        ImGui::SameLine();
        if (sb_ack)  ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "ACK");
        else         ImGui::TextColored(ImVec4(0.3f, 0.3f, 0.3f, 1.0f), "ACK");
    }

    ImGui::SameLine((float)g_Width - 120);
    ImGui::Text("%.1f FPS", io.Framerate);

    ImGui::End();
}

// ========== Window Creation ==========

static bool CreateDeviceWGL(HWND hWnd) {
    HDC hDc = ::GetDC(hWnd);
    PIXELFORMATDESCRIPTOR pfd = {0};
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;

    const int pf = ::ChoosePixelFormat(hDc, &pfd);
    if (pf == 0) return false;
    if (::SetPixelFormat(hDc, pf, &pfd) == FALSE) return false;
    ::ReleaseDC(hWnd, hDc);

    g_hDC = ::GetDC(hWnd);
    if (!g_hRC)
        g_hRC = wglCreateContext(g_hDC);
    return true;
}

static void CleanupDeviceWGL() {
    wglMakeCurrent(nullptr, nullptr);
    if (g_hDC) {
        ::ReleaseDC(g_hWnd, g_hDC);
        g_hDC = nullptr;
    }
}

static bool g_in_modal_move = false;

static LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg) {
    case WM_ENTERSIZEMOVE:
        g_in_modal_move = true;
        return 0;
    case WM_EXITSIZEMOVE:
        g_in_modal_move = false;
        return 0;
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED) {
            g_Width = LOWORD(lParam);
            g_Height = HIWORD(lParam);
        }
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU)
            return 0;
        break;
    case WM_CLOSE:
        g_gui_state.request_shutdown.store(true);
        return 0;
    case WM_DESTROY:
        ::PostQuitMessage(0);
        return 0;
    }
    return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}

// ========== Public Interface ==========

int gui_init() {
    // Load settings from INI file
    std::string config_path = getDefaultConfigPath();
    g_settings.load(config_path);

    // Hide console window if setting is enabled
    if (g_settings.hide_console) {
        FreeConsole();
    }

    // Apply loaded gain settings to GUI state
    g_gui_state.tx_gain_db.store(g_settings.tx_gain_db);
    g_gui_state.rx_gain_db.store(g_settings.rx_gain_db);
    g_gui_state.gains_locked.store(g_settings.gains_locked);

    // Initialize slider tracking
    s_last_tx_gain = (float)g_settings.tx_gain_db;
    s_last_rx_gain = (float)g_settings.rx_gain_db;

    // Make process DPI aware
    ImGui_ImplWin32_EnableDpiAwareness();

    // Create window class
    WNDCLASSEXW wc = {sizeof(wc), CS_OWNDC, WndProc, 0L, 0L,
                      GetModuleHandle(nullptr), nullptr, nullptr, nullptr, nullptr,
                      L"MercuryHFModem", nullptr};
    ::RegisterClassExW(&wc);

    // Create window
    g_hWnd = ::CreateWindowW(wc.lpszClassName, L"Mercury HF Modem",
                             WS_OVERLAPPEDWINDOW,
                             100, 100, 850, 720,
                             nullptr, nullptr, wc.hInstance, nullptr);

    if (!CreateDeviceWGL(g_hWnd)) {
        CleanupDeviceWGL();
        ::DestroyWindow(g_hWnd);
        ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return -1;
    }
    wglMakeCurrent(g_hDC, g_hRC);

    // Enable vsync to limit FPS
    typedef BOOL(WINAPI* PFNWGLSWAPINTERVALEXTPROC)(int interval);
    PFNWGLSWAPINTERVALEXTPROC wglSwapIntervalEXT = (PFNWGLSWAPINTERVALEXTPROC)wglGetProcAddress("wglSwapIntervalEXT");
    if (wglSwapIntervalEXT) {
        wglSwapIntervalEXT(1);  // 1 = vsync on, 0 = vsync off
        printf("VSync enabled\n");
    } else {
        printf("Warning: VSync not available (wglSwapIntervalEXT not found)\n");
    }

    // Show window
    ::ShowWindow(g_hWnd, SW_SHOWDEFAULT);
    ::UpdateWindow(g_hWnd);

    // Setup ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    // Setup style
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 0.0f;
    style.FrameRounding = 3.0f;

    // Setup backends
    ImGui_ImplWin32_InitForOpenGL(g_hWnd);
    ImGui_ImplOpenGL3_Init();

    // Initialize waterfall
    g_waterfall.init();

    return 0;
}

int gui_main_loop() {
    bool done = false;

    while (!done && !g_gui_state.request_shutdown.load()) {
        // Poll messages
        MSG msg;
        while (::PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            ::TranslateMessage(&msg);
            ::DispatchMessage(&msg);
            if (msg.message == WM_QUIT)
                done = true;
        }
        if (done) break;

        if (::IsIconic(g_hWnd) || g_in_modal_move) {
            ::Sleep(10);
            continue;
        }

        // Start ImGui frame
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        // Render our GUI
        RenderGUI();

        // Render
        ImGui::Render();
        glViewport(0, 0, g_Width, g_Height);
        glClearColor(0.1f, 0.1f, 0.12f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        // Present
        ::SwapBuffers(g_hDC);
    }

    g_gui_state.gui_running.store(false);
    return 0;
}

void gui_shutdown() {
    // Save settings before exit
    std::string config_path = getDefaultConfigPath();
    g_settings.tx_gain_db = g_gui_state.tx_gain_db.load();
    g_settings.rx_gain_db = g_gui_state.rx_gain_db.load();
    g_settings.gains_locked = g_gui_state.gains_locked.load();
    g_settings.save(config_path);

    // Shutdown waterfall
    g_waterfall.shutdown();

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupDeviceWGL();
    if (g_hRC) {
        wglDeleteContext(g_hRC);
        g_hRC = nullptr;
    }
    if (g_hWnd) {
        ::DestroyWindow(g_hWnd);
        g_hWnd = nullptr;
    }

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpszClassName = L"MercuryHFModem";
    ::UnregisterClassW(wc.lpszClassName, GetModuleHandle(nullptr));
}

// GUI thread entry point (called from main.cc)
void* gui_thread_func(void* arg) {
    if (gui_init() != 0) {
        printf("Failed to initialize GUI\n");
        return nullptr;
    }

    gui_main_loop();
    gui_shutdown();

    return nullptr;
}
