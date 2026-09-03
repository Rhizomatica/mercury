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

/* ---- Pattern ACK (Welch-Costas tone burst) helpers ----
 * A pattern ACK is a short tone burst — no preamble, no LDPC.  The datalink
 * layer (ARQ) emits one via send_pattern_ack() and detects incoming ones in
 * the RX baseband.  pattern_kind: 0 = plain ACK, 1 = ACK+TURN (break). */

/* Number of pattern symbols (for buffer sizing / airtime). */
int mfsk_pattern_nsymb(void);

/* Max int16 passband samples a pattern TX produces (buffer sizing). */
int mfsk_pattern_max_tx_samples(void);

/* Generate the pattern as int16 passband; returns the sample count. */
int mfsk_pattern_tx(int16_t *out, int pattern_kind);

/* Detect a pattern ACK in an int16 passband chunk.  Returns 1 on a match and
 * sets *is_break (1 = break/ACK+TURN, 0 = plain ACK); 0 if none. */
int mfsk_pattern_detect(const int16_t *pb, int n, int *is_break);

#endif /* MERCURY_MODEM_MFSK_H */
