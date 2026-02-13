/**
 * @file setup_dialog.h
 * @brief Main setup/configuration dialog
 */

#ifndef SETUP_DIALOG_H_
#define SETUP_DIALOG_H_

#include <string>

/**
 * @class SetupDialog
 * @brief Tabbed dialog for all Mercury settings
 */
class SetupDialog {
public:
    SetupDialog();
    ~SetupDialog();

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
     * @brief Load current settings into dialog
     */
    void loadSettings();

private:
    bool is_open_;
    int current_tab_;

    // Render individual tabs
    void renderStationTab();
    void renderNetworkTab();
    void renderARQTab();
    void renderGearShiftTab();
    void renderAdvancedTab();

    // Temporary settings (before Apply)
    // Station
    char my_callsign_[32];
    int radio_type_;  // 0=stockhf, 1=sbitx

    // Network
    int control_port_;
    int data_port_;

    // ARQ
    int connection_timeout_ms_;
    int link_timeout_ms_;
    int max_connection_attempts_;
    bool exit_on_disconnect_;

    // PTT Timing
    int ptt_on_delay_ms_;   // Delay after PTT key before audio starts
    int ptt_off_delay_ms_;  // Delay after audio ends before PTT unkey
    int pilot_tone_ms_;     // Duration of pilot tone before OFDM (0=disabled)
    int pilot_tone_hz_;     // Frequency of pilot tone (250=out of band)

    // Gear Shift
    bool gear_shift_enabled_;
    int initial_config_;
    int ldpc_iterations_max_;

    // Modem
    bool coarse_freq_sync_enabled_;
    bool robust_mode_enabled_;

    // Advanced
    bool hide_console_;
};

// Global dialog - Meyer's Singleton to avoid static init order fiasco
SetupDialog& get_setup_dialog();
#define g_setup_dialog (get_setup_dialog())

#endif // SETUP_DIALOG_H_
