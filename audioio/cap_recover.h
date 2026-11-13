/* What the capture thread does when its device stops delivering samples.
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
 * The capture thread reopens a device that has been silent for its stall
 * window (radio_capture_thread).  A reopen that opens says nothing about the
 * device: on air (K7EK, Windows, WASAPI, 2026-11-10) a USB codec dropped and
 * came back under a new endpoint id, and Mercury reopened the old one 1380
 * times over 47 min, each open "succeeding", with no samples and nothing to
 * tell the operator.  After a restart the old id was gone ("Element not
 * found") and the device had to be selected again.
 *
 * Kept apart from audioio.c, which needs a sound system to run, so the
 * decisions can be unit-tested (tests/audioio/test_cap_recover.c).
 */

#ifndef CAP_RECOVER_H
#define CAP_RECOVER_H

#include <stdbool.h>
#include <stdint.h>
#include "audio_dev_limits.h"

/* Stalls in a row, with no sample between them, before the capture is
 * reported dead (audio health FAILED) and the device looked up again by
 * name.  Each stall is a reopen: the first is a wedged device, which one
 * reopen fixes; the third comes after two reopens that brought nothing,
 * about 6 s of silence from a device that always delivers frames. */
#define CAP_DRY_REOPENS_DEAD 3
/* ...and, once dead, how often the lookup is repeated (every Nth reopen). */
#define CAP_RESOLVE_EVERY    5

typedef struct {
    unsigned dry;     /* stalls since the last sample, not counting those keyed */
    bool     dead;    /* reported dead, not yet delivering again */
} cap_recover_t;

enum {
    CAP_REC_DEAD    = 1 << 0,   /* report the capture dead now */
    CAP_REC_RESOLVE = 1 << 1,   /* look the device up by name before reopening */
};

/* The device has stalled and is about to be reopened.  keyed: we are
 * transmitting, and a half-duplex codec delivers nothing then (an MFSK floor
 * frame is 27 s): not counted.  Returns CAP_REC_*. */
int  cap_recover_on_stall(cap_recover_t *r, bool keyed);
/* Samples arrived.  True when the capture had been reported dead: it is
 * running again. */
bool cap_recover_on_data(cap_recover_t *r);

enum {
    CAP_PICK_PRESENT   = -1,   /* the current id is still enumerated: keep it */
    CAP_PICK_NONE      = -2,   /* no device of that name */
    CAP_PICK_AMBIGUOUS = -3,   /* more than one: do not guess which radio */
};

/* The device to reopen when cur_id may have gone: the index of the one device
 * named name (case-insensitive, exact), or a CAP_PICK_* value.  name empty:
 * CAP_PICK_NONE. */
int cap_pick_by_name(const char *cur_id, const char *name,
                     const char (*ids)[AUDIO_DEV_STR_MAX],
                     const char (*names)[AUDIO_DEV_STR_MAX], int n);

#endif
