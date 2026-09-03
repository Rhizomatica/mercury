/* Audio subsystem
 *
 * Copyright (C) 2024 Rhizomatica
 * Author: Rafael Diniz <rafael@rhizomatica.org>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 */

#pragma once

#include <ffbase/string.h>
#include "audio_dev_limits.h"

#include <stdio.h>
#include <sys/types.h>

#include <fcntl.h>

#include "ring_buffer_posix.h"

#define AUDIO_SUBSYSTEM_ALSA 0
#define AUDIO_SUBSYSTEM_PULSE 1
#define AUDIO_SUBSYSTEM_WASAPI 2
#define AUDIO_SUBSYSTEM_DSOUND 3
#define AUDIO_SUBSYSTEM_COREAUDIO 4
#define AUDIO_SUBSYSTEM_OSS 5
#define AUDIO_SUBSYSTEM_AAUDIO 6
#define AUDIO_SUBSYSTEM_SHM 7
#define AUDIO_SUBSYSTEM_NULL 8
#define AUDIO_SUBSYSTEM_FIFO 9
#define AUDIO_SUBSYSTEM_SOCK 10

#define LEFT 0
#define RIGHT 1
#define STEREO 2

extern cbuf_handle_t capture_buffer;
extern cbuf_handle_t playback_buffer;


int audioio_init_internal(char *capture_dev, char *playback_dev, int audio_subsys, int capture_channel_layout, pthread_t *radio_capture,
						  pthread_t *radio_playback);

int audioio_init_buffers(void);
void audioio_deinit_buffers(void);

/* --- Audio path health -------------------------------------------------
 *
 * A bad device choice must NOT take the process down: the operator may well be
 * about to pick a different card (audioio_restart, or the UI device list), and
 * a mercury that exits leaves them nothing to pick with.  So the capture and
 * playback threads report their state here instead, and the UI surfaces it.
 *
 * Without this the failure is invisible: the thread logs, returns, and the
 * status the UI publishes still describes a perfectly healthy mercury while
 * nothing is being heard or transmitted. */
typedef enum {
    AUDIO_HEALTH_STOPPED = 0,  /* not started, or shut down cleanly        */
    AUDIO_HEALTH_RUNNING,      /* stream is up                             */
    AUDIO_HEALTH_FAILED        /* could not start / died; reason is set    */
} audio_health_t;

audio_health_t audioio_capture_health(void);
audio_health_t audioio_playback_health(void);

/* Human-readable reason for the most recent failure on either path, e.g.
 * "capture: device negotiated 44100 Hz (try plughw:...)".  Empty when
 * nothing has failed.  Copies into buf; always NUL-terminates. */
void audioio_health_reason(char *buf, size_t buflen);

/* Convenience for status reporting: true when NEITHER path has failed.  On
 * failure `reason` receives the message.  Primitive types only, so a caller
 * can declare it extern rather than include this header (which drags in
 * ffbase) -- the convention ui_communication.c already uses for
 * audioio_restart. */
bool audioio_health_ok(char *reason, size_t reasonlen);

/* Reset both health flags to STOPPED and clear the reason.  audioio_restart
 * calls this before spawning the new threads so a subsequent
 * audioio_wait_healthy() observes the new run, not the one that just stopped. */
void audioio_health_reset(void);

/* Wait up to timeout_ms for the capture and playback paths to leave STOPPED.
 * Returns:
 *   0  - both paths reached RUNNING (the restart is healthy);
 *   -1 - at least one path reached FAILED (reason via audioio_health_reason);
 *   -2 - timed out with one or both still STOPPED (indeterminate; the
 *        null/fifo/sock backends never set health). */
int audioio_wait_healthy(int timeout_ms);

/* Stop the running audio threads and start them again with a new subsystem,
 * channel layout, and/or device selection.  The buffers are cleared but never
 * destroyed.
 *
 * capture_dev / playback_dev semantics:
 *   - NULL        -> keep the device currently in use.
 *   - empty ("")  -> clear it; the (new) subsystem resolves its own default.
 *   - non-empty   -> use this device id/name (resolved in place).
 *
 * A switch to a subsystem that fails to open leaves the audio path stopped
 * (the failure is reported via audioio_health_*, not by this function's
 * return value), so the caller must be prepared to switch back. */
int audioio_restart(const char *capture_dev, const char *playback_dev,
                    int audio_subsys, int capture_channel_layout);

int audioio_deinit(pthread_t *radio_capture, pthread_t *radio_playback);
int audioio_pick_default_subsystem(void);

int tx_transfer(double *buffer, size_t len);
int rx_transfer(double *buffer, size_t len);


void list_soundcards(int audio_system);

// Enumerate device names and IDs into caller-supplied buffers.
// mode: 0 = FFAUDIO_DEV_PLAYBACK, 1 = FFAUDIO_DEV_CAPTURE
// Returns the number of devices found (up to max_count).
// Each entry in ids[] and dev_names[] will be a NUL-terminated string.
int get_soundcard_list(int audio_system, int mode,
                       char ids[][AUDIO_DEV_STR_MAX], char dev_names[][AUDIO_DEV_STR_MAX], int max_count);
