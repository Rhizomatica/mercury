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
 * the RX baseband.  pattern_kind (bit-packed):
 *   0                              = plain ACK (bare, fringe path — unchanged)
 *   1                              = ACK+TURN break (bare)
 *   MFSK_PATTERN_TAGGED|epoch<<1|b = fast windowed ACK: base ACK/break (bit b)
 *                                    + a short appended Welch-Costas mini-pattern
 *                                    encoding the 2-bit epoch, so a clean
 *                                    multi-block burst can be acked by the fast
 *                                    pattern instead of the 3.74 s coded frame
 *                                    (Phase 2c). */
#define MFSK_PATTERN_TAGGED 0x80

/* Number of BASE pattern symbols (detect anchor; airtime of a bare pattern). */
int mfsk_pattern_nsymb(void);

/* Max int16 passband samples a pattern TX produces — includes the optional
 * epoch symbol, so RX windows sized by this always contain it (buffer sizing). */
int mfsk_pattern_max_tx_samples(void);

/* Generate the pattern as int16 passband; returns the sample count. */
int mfsk_pattern_tx(int16_t *out, int pattern_kind);

/* Detect a pattern ACK in an int16 passband chunk.
 *   returns 0 = no pattern found;
 *   returns 1 = decided: *out_kind is the pattern_kind (0/1 bare, or
 *               MFSK_PATTERN_TAGGED|epoch<<1|break) — caller consumes the window;
 *   returns 2 = base pattern found but its trailing epoch mini-pattern has not
 *               fully arrived in the window yet (only when expect_epoch != 0) —
 *               caller must WAIT (accumulate more, do NOT consume): deciding now
 *               would mis-read a still-arriving tagged ACK as bare.
 * expect_epoch: 1 when epoch-tagged fast ACKs are possible (MERCURY_FAST_ACK) —
 * enables the return-2 wait.  0 keeps the legacy immediate decision (no added
 * latency on the bare-only fringe path). */
int mfsk_pattern_detect(const int16_t *pb, int n, int expect_epoch, int *out_kind);

#endif /* MERCURY_MODEM_MFSK_H */
