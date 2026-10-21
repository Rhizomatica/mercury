/* The host's broadcast carousel (bcast_carousel.c)
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef BCAST_CAROUSEL_H_
#define BCAST_CAROUSEL_H_

#include <stddef.h>
#include <stdint.h>

#define BCAST_CAROUSEL_BAD   (-1)   /* the mode cannot carry files, a bad name, too big */
#define BCAST_CAROUSEL_BUSY  (-2)   /* one is sending, or an ARQ session is up         */

/* Send data, filed under name, at the listen mode: cycles cycles (one frame
 * per RaptorQ block each), 0 until stopped.  Returns at once; progress in bcast_carousel_get (broadcast.h). */
int  bcast_carousel_send(const char *name, const uint8_t *data, size_t len, int cycles,
                         char *err, size_t errlen);
/* Stop it at the next frame (and wait for its thread); no-op if none. */
void bcast_carousel_stop(void);

#endif /* BCAST_CAROUSEL_H_ */
