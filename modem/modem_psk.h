/* PSK modem backend -- the few-carrier differential QPSK rung (modem/psk)
 * behind the modem_backend_t vtable.
 *
 * Three carriers (900/1500/2100 Hz) of differential QPSK at 100 baud, no
 * preamble: the receiver finds a frame from its marker symbols and the data's
 * fourth power, and decodes it differentially, so nothing depends on a
 * channel estimate.  It presents the same frame contract as the other
 * backends (payload + 2-byte CRC16 in, bytes out only when the CRC is good).
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef MERCURY_MODEM_PSK_H
#define MERCURY_MODEM_PSK_H

#include "modem_backend.h"

/* Mode integer, clear of the FreeDV enum and of the MFSK modes (100..104):
 * rung 2 of the BW2300 and BW2750 ladders. */
#define MERCURY_MODE_PSK3 110

static inline int mercury_mode_is_psk(int mode)
{
    return mode == MERCURY_MODE_PSK3;
}

extern const modem_backend_t modem_backend_psk;

#endif
