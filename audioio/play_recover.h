/* What the playback thread does when its device stops taking audio.
 *
 * Copyright (C) 2026 Rhizomatica
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
 * ---------------------------------------------------------------------------
 *
 * The modem keys PTT, hands the burst to the playback ring and unkeys when the
 * burst's airtime has passed by the clock (modem.c): nothing there waits on
 * the device.  A playback device that stops taking audio therefore leaves
 * every later burst keyed over silence.  Before this, a write that failed was
 * logged and its period dropped, the next one failed the same way, and so on
 * until Mercury was restarted (issue #591: PulseAudio on PipeWire, a USB
 * codec, "ptt is on but there is no audio at all").
 *
 * The playback thread now reopens a device that has taken nothing for
 * PLAY_STALL_MS while it had audio to give it, as the capture thread does
 * (cap_recover.h).  Kept apart from audioio.c, which needs a sound system to
 * run, so the decisions can be unit-tested (tests/audioio/test_play_recover.c).
 */

#ifndef PLAY_RECOVER_H
#define PLAY_RECOVER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "ring_buffer_posix.h"

/* How long a device may take none of the audio offered before it is reopened.
 * The device buffer is tens of ms, so a healthy one takes something every
 * period; a second is a wedged or failed stream, not a full buffer. */
#define PLAY_STALL_MS 1000

/* Reopens in a row, with no audio taken between them, before playback is
 * reported dead (audio health FAILED).  A failed or wedged stream is cured by
 * the first; the third follows two that brought nothing. */
#define PLAY_DRY_REOPENS_DEAD 3

typedef struct {
    bool     stuck;      /* the device has taken nothing since stuck_ms */
    uint64_t stuck_ms;
    unsigned dry;        /* reopens since the device last took audio */
    bool     dead;       /* reported dead, not yet taking audio again */
} play_recover_t;

enum {
    PLAY_REC_BACK  = 1 << 0,   /* first audio taken after a reopen: say so */
    PLAY_REC_ALIVE = 1 << 1,   /* it had been reported dead: running again */
};

/* The device took audio.  Returns PLAY_REC_*. */
int  play_recover_on_write(play_recover_t *r);
/* The device took none (a write error, or no room on a non-blocking stream)
 * at now_ms.  True when that has lasted PLAY_STALL_MS: reopen it now. */
bool play_recover_on_stuck(play_recover_t *r, uint64_t now_ms);
/* A reopen was made, whether the device opened or not.  True, once, when
 * playback is to be reported dead. */
bool play_recover_on_reopen(play_recover_t *r);

/* Drop everything queued in ring, through scratch.  Returns the bytes
 * dropped.  The playback thread is the ring's only reader. */
size_t play_drop_queued(cbuf_handle_t ring, uint8_t *scratch, size_t scratch_len);

/* The wait between two reopen attempts: ms in short steps, dropping what the
 * modem queues meanwhile, and returning early when stop() says so.
 *
 * The modem keys PTT and then write_buffer()s the burst, which blocks while
 * the ring has no room.  A device that never comes back (a USB codec gone
 * for good) leaves nothing reading the ring, and once its ~384 s filled, the
 * modem's TX thread blocked with PTT keyed (#617 review).  Audio queued
 * while there is no device is late anyway. */
void play_backoff_wait(cbuf_handle_t ring, uint8_t *scratch, size_t scratch_len,
                       unsigned ms, bool (*stop)(void));

#endif
