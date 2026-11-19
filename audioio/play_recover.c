/* What the playback thread does when its device stops taking audio
 * (play_recover.h).
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
 */

#include "play_recover.h"
#include "os_interop.h"

int play_recover_on_write(play_recover_t *r)
{
    int out = 0;
    if (r->dry > 0)
        out |= PLAY_REC_BACK;
    if (r->dead)
        out |= PLAY_REC_ALIVE;
    r->stuck = false;
    r->dry = 0;
    r->dead = false;
    return out;
}

bool play_recover_on_stuck(play_recover_t *r, uint64_t now_ms)
{
    if (!r->stuck)
    {
        r->stuck = true;
        r->stuck_ms = now_ms;
        return false;
    }
    return now_ms - r->stuck_ms >= PLAY_STALL_MS;
}

bool play_recover_on_reopen(play_recover_t *r)
{
    /* The stall window starts again from the reopen. */
    r->stuck = false;
    r->dry++;
    if (r->dead || r->dry < PLAY_DRY_REOPENS_DEAD)
        return false;
    r->dead = true;
    return true;
}

size_t play_drop_queued(cbuf_handle_t ring, uint8_t *scratch, size_t scratch_len)
{
    size_t dropped = 0;
    size_t late = size_buffer(ring);
    while (late > 0)
    {
        size_t k = late < scratch_len ? late : scratch_len;
        read_buffer(ring, scratch, k);
        late -= k;
        dropped += k;
    }
    return dropped;
}

/* Short enough that the modem is never kept waiting on a full ring for
 * long, and a shutdown during the backoff is still prompt. */
#define PLAY_BACKOFF_STEP_MS 100

void play_backoff_wait(cbuf_handle_t ring, uint8_t *scratch, size_t scratch_len,
                       unsigned ms, bool (*stop)(void))
{
    for (unsigned slept = 0; slept < ms && !stop(); slept += PLAY_BACKOFF_STEP_MS)
    {
        play_drop_queued(ring, scratch, scratch_len);
        hermes_usleep(PLAY_BACKOFF_STEP_MS * 1000);
    }
    play_drop_queued(ring, scratch, scratch_len);
}
