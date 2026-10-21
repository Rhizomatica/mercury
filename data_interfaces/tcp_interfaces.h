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

#ifndef TCP_INTERFACES_H_
#define TCP_INTERFACES_H_

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define DEFAULT_ARQ_PORT 8300
#define DEFAULT_BROADCAST_PORT 8100

#define TCP_BLOCK_SIZE 128

int interfaces_init(int arq_tcp_base_port, int broadcast_tcp_port, size_t broadcast_frame_size);

/* Re-frame the broadcast TCP plane after a listen-mode change.  The connected
 * client (the server serves one at a time) picks the new size up on its next
 * frame; its buffers are sized for the largest selectable frame, so no
 * reallocation is needed. */
void interfaces_set_broadcast_frame_size(size_t broadcast_frame_size);
size_t interfaces_get_broadcast_frame_size(void);
void interfaces_shutdown();

// ARQ TCP/IP server threads
void *server_worker_thread_ctl(void *port);

/* Configure TNC control-port pacing (from [tnc] INI keys; clamped). */
void tnc_set_intervals(int keepalive_s, int buffer_report_ms);

// ARQ control + data port reactor (started by interfaces_init)
void *arq_reactor_thread(void *port);

// BROADCAST TCP/IP server threads
void *send_thread(void *client_socket_ptr);
void *recv_thread(void *client_socket_ptr);
void *tcp_server_thread(void *port_ptr);

// TNC / radio functions
void ptt_on();
void ptt_off();
void tnc_send_pending();
void tnc_send_cancelpending();
void tnc_send_connected();
void tnc_send_cqframe(const char *source_call, int bw_hz);
void tnc_send_disconnected();
void tnc_send_buffer(uint32_t bytes);
/* A broadcast chat message from the host program, at the listen mode's frame
 * size: 0, -1 with no broadcast frame size yet, -2 too long for one frame. */
int broadcast_send_message(const uint8_t *msg, size_t len);
/* The longest such message at the listen mode, 0 with none set yet. */
size_t broadcast_message_max(void);
/* Every broadcast frame the modem decodes: stores received chat, gives the
 * frame to the host's tap and, while one is attached, to the TCP client. */
void broadcast_rx_frame(const uint8_t *frame, size_t len, int mode, float snr);
/* The host program's tap on those frames (NULL: none).  Called on the
 * modem's RX thread: copy and return. */
void broadcast_set_rx_tap(void (*tap)(const uint8_t *frame, size_t len, int mode, float snr));
/* Is a periodic BUFFER report due: the count changed, or -- in a session,
 * with bytes still to confirm -- no BUFFER line went for
 * TNC_BUFFER_REPEAT_MS (50 s)?  (#397) */
bool tnc_buffer_report_due(int buffered, int last_reported, bool connected,
                           uint64_t now_ms, uint64_t last_sent_ms);
void tnc_send_registered(const char *callsign);
void tnc_send_sn(float snr);
void tnc_send_busy(bool busy);
void tnc_send_bitrate(uint32_t speed_level, uint32_t bps);
/* Report a frame decoded by the passive monitor to the host, VARA-monitor
 * style: "MONITOR <mode> <kind> <detail> SNR=<snr>\r".  Lossy (display-only). */
void tnc_send_monitor(const char *mode_name, const char *kind,
                      const char *detail, float snr_db);

// Getters for cached telemetry (thread-safe reads of last reported values)
float tnc_get_last_snr(void);
uint32_t tnc_get_last_bitrate_bps(void);

#endif // TCP_INTERFACES_H_
