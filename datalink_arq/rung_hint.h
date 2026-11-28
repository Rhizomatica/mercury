#ifndef RUNG_HINT_H
#define RUNG_HINT_H

#include <stdbool.h>
#include "carousel.h"

/* Per-peer rung memory across sessions (docs/EFFICIENCY-FOLLOWUPS.md, A):
 * what each rung delivered last session, keyed by peer callsign, in a small
 * flat file (the session's own frames outvote it; see car_rung_seed). */

/* Fill hint with peer's stored counts; false when there is none (no file,
 * no line for the peer, nothing worth seeding). */
bool rung_hint_load(const char *peer, car_rung_hint_t *h);
/* Store hint under peer (rewrites the file; the rest of it is kept). */
void rung_hint_store(const char *peer, const car_rung_hint_t *h);

/* Where the file is.  Default: the station's state directory, beside the
 * message store (message_store.c).  MERCURY_RUNG_HINTS overrides it -- the
 * sim and the tests use that for isolated A/B runs. */
void rung_hint_set_path(const char *path);

#endif
