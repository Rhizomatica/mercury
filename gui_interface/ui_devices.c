/* HERMES Modem — UI device-list helpers
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Kept in its own translation unit so the logic can be unit-tested without
 * linking the websocket server and the whole UI context.
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui_status.h"
#include "ui_communication.h"

/* Make every display name in the list distinguishable.
 *
 * Two of the same USB codec present identical human-readable names — a
 * PCM2901 in an IC-7300 and another in an IC-9700 are both "PCM2901 Audio
 * Codec Analog Stereo" — while their ids differ.  Every UI here is
 * label-driven: the dropdown shows names, and the selection is mapped back to
 * an id by matching the label.  With duplicate labels the operator cannot tell
 * the two radios apart, and whichever they pick resolves to the first match,
 * so one of the two devices is unreachable (issue #189).
 *
 * Only names that actually collide get the id appended, so the ordinary
 * one-radio case still reads as plain English.  Doing it here rather than in
 * each UI keeps the embedded, web and remote lists showing the same labels. */
void ui_devices_disambiguate(ui_device_t *devs, int count)
{
    if (!devs || count <= 1)
        return;

    /* Decide everything against the ORIGINAL names before touching any of
     * them.  Renaming inside the scan makes each device stop matching the one
     * it collided with, so only the first of a colliding group is ever
     * labelled and its partner keeps the bare name — which is the same bug
     * from the other side. */
    bool *needs_id = calloc((size_t)count, sizeof(*needs_id));
    if (!needs_id)
        return;   /* leave the list readable rather than half-relabelled */

    for (int i = 0; i < count; i++)
    {
        for (int j = i + 1; j < count; j++)
        {
            if (strcmp(devs[i].name, devs[j].name) == 0)
            {
                needs_id[i] = true;
                needs_id[j] = true;
            }
        }
    }

    for (int i = 0; i < count; i++)
    {
        /* A device with no id cannot be told apart; an empty "[]" would just
         * be noise. */
        if (!needs_id[i] || devs[i].id[0] == '\0')
            continue;

        /* The id is the part that distinguishes these entries, so it is the
         * part that must survive.  Appending it and letting snprintf trim the
         * result would truncate from the RIGHT, cutting the id — and two ids
         * that differ only past the cut then produce byte-identical labels
         * again, silently undoing this whole function.  Give the id its full
         * width and spend whatever is left on the name. */
        char label[UI_DEV_NAME_MAX];
        const size_t cap = sizeof(label);          /* includes the NUL */
        const size_t idl = strlen(devs[i].id);
        const size_t deco = 3;                      /* " [" and "]" */

        if (idl + deco + 1 <= cap)
        {
            snprintf(label, cap, "%.*s [%s]",
                     (int)(cap - 1 - deco - idl), devs[i].name, devs[i].id);
        }
        else
        {
            /* Pathological: the id alone overflows the field.  Drop the name
             * entirely and keep the id's TAIL — a truncated head is what ids
             * share, a tail is where they differ. */
            snprintf(label, cap, "[%s]", devs[i].id + (idl - (cap - deco)));
        }
        snprintf(devs[i].name, sizeof(devs[i].name), "%s", label);
    }

    free(needs_id);
}
