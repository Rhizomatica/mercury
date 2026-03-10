/* HERMES Modem
 *
 * Copyright (C) 2025 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3, or (at your option)
 * any later version.
 *
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 */

#ifndef ARQ_H_
#define ARQ_H_

#define CALLSIGN_MAX_SIZE 16 

#define ARQ_BANDWIDTH_NARROW_HZ 500
#define ARQ_BANDWIDTH_FULL_HZ   2300

#define RX 0
#define TX 1

#define HEADER_SIZE 1

#define PACKET_ARQ_CONTROL       0x00
#define PACKET_ARQ_DATA          0x01
#define PACKET_ARQ_CALL          0x02
#define PACKET_BROADCAST_CONTROL 0x03
#define PACKET_BROADCAST_PAYLOAD 0x04

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fsm.h"
#include "arq_events.h"

/** @brief Runtime ARQ connection and mode state shared across modules. */
typedef struct
{
    int TRX; // RX (0) or TX (1)
    char my_call_sign[CALLSIGN_MAX_SIZE];
    char src_addr[CALLSIGN_MAX_SIZE], dst_addr[CALLSIGN_MAX_SIZE];
    bool encryption;
    int call_burst_size;
    bool listen;
    int bw; // in Hz
    int retry_slots; // 0 = use compiled default
    size_t frame_size;
    int mode;
} arq_info;

/** @brief Action types emitted by ARQ for modem worker execution. */
typedef enum
{
    ARQ_ACTION_NONE = 0,
    ARQ_ACTION_TX_CONTROL = 1,
    ARQ_ACTION_TX_PAYLOAD = 2,
    ARQ_ACTION_MODE_SWITCH = 3
} arq_action_type_t;

/** @brief Single modem action item popped by modem TX worker. */
typedef struct
{
    arq_action_type_t type;
    int mode;
    size_t frame_size;
} arq_action_t;

/** @brief Snapshot of current ARQ runtime state for telemetry/decision making. */
typedef struct
{
    bool initialized;
    bool connected;
    int trx;
    int tx_backlog_bytes;
    int speed_level;
    int payload_mode;    /* my TX mode (ISS direction)                     */
    int peer_tx_mode;    /* RX decoder mode = peer's TX mode (IRS direction) */
    int control_mode;
    int preferred_rx_mode;
    int preferred_tx_mode;
} arq_runtime_snapshot_t;

extern arq_info arq_conn;
extern fsm_handle arq_fsm;

/**
 * @brief Initialize ARQ subsystem.
 * @param frame_size Active modem frame size in bytes.
 * @param mode Initial modem mode.
 * @return 0 on success, non-zero on failure.
 */
int arq_init(size_t frame_size, int mode);

/**
 * @brief Shut down ARQ workers and release ARQ resources.
 */
void arq_shutdown();

/**
 * @brief Execute 1 Hz ARQ maintenance tick.
 */
void arq_tick_1hz(void);

/**
 * @brief Post an FSM event to ARQ.
 * @param event Event identifier from fsm.h.
 */
void arq_post_event(int event);

/**
 * @brief Check whether ARQ link is connected.
 * @return true when connected; otherwise false.
 */
bool arq_is_link_connected(void);

/**
 * @brief Get the effective ARQ bandwidth cap in Hz.
 * @return 500 for narrow-band mode, otherwise 2300.
 */
int arq_effective_bandwidth_hz(void);

/**
 * @brief Check whether the current ARQ bandwidth cap allows a modem mode.
 * @param mode FreeDV mode value.
 * @return true if the mode is allowed under the active BW setting.
 */
bool arq_bandwidth_allows_mode(int mode);

/**
 * @brief Queue outbound payload bytes for ARQ transmission.
 * @param data Pointer to payload bytes.
 * @param len Number of bytes to queue.
 * @return Number of bytes queued, or negative on error.
 */
int arq_queue_data(const uint8_t *data, size_t len);

/**
 * @brief Get pending outbound payload backlog.
 * @return Backlog size in bytes.
 */
int arq_get_tx_backlog_bytes(void);

/**
 * @brief Get current ARQ speed level (gear).
 * @return Speed level index.
 */
int arq_get_speed_level(void);

/**
 * @brief Get active ARQ payload mode.
 * @return FreeDV payload mode value.
 */
int arq_get_payload_mode(void);

/**
 * @brief Get active ARQ control mode.
 * @return FreeDV control mode value.
 */
int arq_get_control_mode(void);

/**
 * @brief Get ARQ-preferred receive mode for modem.
 * @return FreeDV mode value.
 */
int arq_get_preferred_rx_mode(void);

/**
 * @brief Get ARQ-preferred transmit mode for modem.
 * @return FreeDV mode value.
 */
int arq_get_preferred_tx_mode(void);

/**
 * @brief Inform ARQ of modem mode/frame size actually active in modem.
 * @param mode Active FreeDV mode.
 * @param frame_size Active frame size in bytes.
 */
void arq_set_active_modem_mode(int mode, size_t frame_size);

/**
 * @brief Handle incoming compressed CALL/ACCEPT frame.
 * @param data Frame bytes.
 * @param frame_size Frame length in bytes.
 * @return true if frame was handled by ARQ connect path.
 */
bool arq_handle_incoming_connect_frame(uint8_t *data, size_t frame_size);

/**
 * @brief Handle incoming regular ARQ control/data frame.
 * @param data Frame bytes.
 * @param frame_size Frame length in bytes.
 * @param rx_snr Local receive SNR at decode time (dB); 0.0 = unknown.
 */
void arq_handle_incoming_frame(uint8_t *data, size_t frame_size, float rx_snr);

/**
 * @brief Feed decoder/link metrics into ARQ adaptation.
 * @param sync Decoder sync flag.
 * @param snr Estimated SNR.
 * @param rx_status Decoder RX status flags.
 * @param frame_decoded True when a frame decoded this cycle.
 */
void arq_update_link_metrics(int sync, float snr, int rx_status, bool frame_decoded);

/**
 * @brief Try to dequeue next modem action without blocking.
 * @param action Output action item.
 * @return true if an action was dequeued.
 */
bool arq_try_dequeue_action(arq_action_t *action);

/**
 * @brief Wait for next modem action.
 * @param action Output action item.
 * @param timeout_ms Wait timeout in milliseconds.
 * @return true if an action was dequeued.
 */
bool arq_wait_dequeue_action(arq_action_t *action, int timeout_ms);

/**
 * @brief Copy ARQ runtime snapshot.
 * @param snapshot Output snapshot pointer.
 * @return true when snapshot contains initialized state.
 */
bool arq_get_runtime_snapshot(arq_runtime_snapshot_t *snapshot);

/**
 * @brief Submit parsed control command from TCP bridge to ARQ.
 * @param cmd Command message.
 * @return 0 on success, negative on queue/error.
 */
int arq_submit_tcp_cmd(const arq_cmd_msg_t *cmd);

/**
 * @brief Submit payload bytes from TCP bridge to ARQ.
 * @param data Payload bytes.
 * @param len Payload length in bytes.
 * @return 0 on success, negative on queue/error.
 */
int arq_submit_tcp_payload(const uint8_t *data, size_t len);

/**
 * @brief Clear legacy ARQ connection state and buffers.
 */
void clear_connection_data();

/**
 * @brief Reset external arq_info structure.
 * @param arq_conn Pointer to structure to reset.
 */
void reset_arq_info(arq_info *arq_conn);

/**
 * @brief Set runtime retry slot counts.
 *
 * Updates CALL, ACCEPT, and DATA retry counters used by the ARQ FSM.
 * DISCONNECT retries are left at the compiled default.
 * Pass 0 to restore all counters to compiled defaults.
 *
 * @param slots  Number of retry slots, or 0 to restore defaults.
 */
void arq_set_retry_slots(int slots);

/**
 * @brief Trigger outgoing call attempt using current ARQ addresses.
 */
void call_remote();

/**
 * @brief Trigger callee-side accept flow in compatibility path.
 */
void callee_accept_connection();

#endif
