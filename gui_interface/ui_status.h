/* HERMES Modem — UI status snapshot
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * One typed description of "what the UI shows", filled by a single gatherer in
 * ui_communication.c and rendered two ways:
 *
 *   - the embedded Fyne UI reads the struct directly across the CGo bridge;
 *   - remote clients (HERMES web UI, mercury-qt) get ui_status_to_json().
 *
 * The point of the struct existing is that both paths read the SAME gathered
 * snapshot, so a field can never mean one thing locally and another remotely.
 * Adding a field means: add it here, set it in the gatherer, and — if remote
 * clients need it — render it in ui_status_to_json(), whose exact output is
 * pinned by tests/gui_interface/test_ui_status.c.
 */
#ifndef UI_STATUS_H
#define UI_STATUS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "arq.h"   /* CALLSIGN_MAX_SIZE */

#define UI_AUDIO_ERR_MAX 192

typedef struct {
    int    bitrate_bps;
    double snr_db;
    char   user_callsign[CALLSIGN_MAX_SIZE];
    char   dest_callsign[CALLSIGN_MAX_SIZE];
    bool   sync;                 /* ARQ session established               */
    bool   transmitting;         /* PTT asserted (direction tx vs rx)     */
    bool   client_tcp_connected; /* a host is attached to the TNC ports   */
    long   bytes_transmitted;
    long   bytes_received;
    float  tx_gain_db;
    float  tx_peak_dbfs;
    bool   waterfall_enabled;
    /* Audio path health.  A bad device choice does not stop mercury -- the
     * operator needs it alive to pick another card -- so the only way they
     * learn the card is wrong is if the UI says so.  Empty audio_error means
     * healthy; otherwise it carries the reason, e.g. an unsupported rate. */
    bool   audio_ok;
    char   audio_error[UI_AUDIO_ERR_MAX];
    char   arq_tx_mode[16];     /* local ARQ payload mode                  */
    char   arq_rx_mode[16];     /* peer payload mode used by local decoder */
    bool   radio_frequency_valid;
    uint64_t radio_frequency_hz;
    uint64_t radio_frequency_age_ms;
} ui_status_t;

/* Render the snapshot as the status JSON remote clients already expect.
 * Returns the number of bytes written (excluding the NUL), or -1 if the
 * buffer is too small.  Pure function: no locks, no globals, unit-testable. */
int ui_status_to_json(const ui_status_t *st, char *buf, size_t buflen);

/* ---- Device enumeration ----------------------------------------------------
 * Audio and radio choices, in one shape for both consumers: the websocket path
 * renders them as JSON, the embedded UI copies the array out over CGo.  Same
 * reason as the status struct — the two must not be able to disagree about
 * what is on the list or which entry is selected. */

/* PulseAudio/PipeWire node names are long: an IC-7300 enumerates as
 * "alsa_input.usb-Burr-Brown_from_TI_USB_Audio_CODEC-00.analog-stereo",
 * 66 characters.  At 64 these were silently cut to 63, which both wrote a
 * broken device into mercury.ini and stopped the id ever matching a real
 * device on the next open -- so the backend fell back to the default card
 * and the radio went deaf (issue #185).  ALSA "plughw:CARD=..." names and
 * WASAPI GUID+description strings run long too. */
#define UI_DEV_ID_MAX   256
#define UI_DEV_NAME_MAX 256

typedef struct {
    char id[UI_DEV_ID_MAX];     /* device id / hamlib model number as text */
    char name[UI_DEV_NAME_MAX]; /* what the operator sees                  */
} ui_device_t;

typedef enum {
    UI_DEV_CAPTURE  = 0,
    UI_DEV_PLAYBACK = 1,
} ui_device_kind_t;

/* Render a device list as the JSON a remote client expects.  `type_name` is
 * the message type ("capture_dev_list", "playback_dev_list", "radio_list").
 * Returns bytes written, or -1 if the buffer is too small. */
int ui_device_list_to_json(const char *type_name, const ui_device_t *devs, int count,
                           const char *selected, char *buf, size_t buflen);

#endif /* UI_STATUS_H */
