/* What the capture thread does when its device stops delivering samples
 * (cap_recover.h).
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

#include "cap_recover.h"

#include <ctype.h>
#include <string.h>

int cap_recover_on_stall(cap_recover_t *r, bool keyed)
{
    if (keyed)
        return 0;
    r->dry++;
    if (r->dry < CAP_DRY_REOPENS_DEAD)
        return 0;
    int f = 0;
    if (!r->dead)
    {
        r->dead = true;
        f |= CAP_REC_DEAD;
    }
    if ((r->dry - CAP_DRY_REOPENS_DEAD) % CAP_RESOLVE_EVERY == 0)
        f |= CAP_REC_RESOLVE;
    return f;
}

bool cap_recover_on_data(cap_recover_t *r)
{
    bool was = r->dead;
    r->dry = 0;
    r->dead = false;
    return was;
}

static bool ieq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return false;
    return *a == *b;
}

int cap_pick_by_name(const char *cur_id, const char *name,
                     const char (*ids)[AUDIO_DEV_STR_MAX],
                     const char (*names)[AUDIO_DEV_STR_MAX], int n)
{
    if (cur_id && cur_id[0])
        for (int i = 0; i < n; i++)
            if (strcmp(cur_id, ids[i]) == 0)
                return CAP_PICK_PRESENT;
    if (!name || !name[0])
        return CAP_PICK_NONE;
    /* Exact names only: a substring could bind another radio's codec, and
     * identical names (two of the same USB codec) are not told apart. */
    int match = CAP_PICK_NONE;
    for (int i = 0; i < n; i++)
        if (ieq(name, names[i]))
        {
            if (match >= 0)
                return CAP_PICK_AMBIGUOUS;
            match = i;
        }
    return match;
}
