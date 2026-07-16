/* MFSK modem backend — non-coherent 32-MFSK weak-signal mode behind the
 * modem_backend_t vtable.
 *
 * This is backend #2: a burst codec (preamble + LDPC-coded MFSK payload +
 * postamble) that reaches ~10 dB below the OFDM data modes.  It presents the
 * same frame contract as FreeDV (opaque payload + 2-byte CRC16 in/out, bytes
 * returned only on CRC-valid) so the datalink layer is unchanged; internally
 * the RX side keeps its own sliding sample window and runs the non-coherent
 * preamble correlator + energy demod + LDPC decode.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef MERCURY_MODEM_MFSK_H
#define MERCURY_MODEM_MFSK_H

#include "modem_backend.h"

/* Mode integer for the MFSK fringe mode.  Chosen well clear of the FreeDV
 * mode enum (which currently tops out at QAM16C2=25) so it can never collide
 * with a codec2 mode; backend_for_mode() routes it to the MFSK backend. */
#define MERCURY_MODE_MFSK 100

extern const modem_backend_t modem_backend_mfsk;

#endif /* MERCURY_MODEM_MFSK_H */
