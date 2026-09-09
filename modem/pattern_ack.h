/* Tone-pattern signalling — the ARQ reverse path
 *
 * A pattern ACK is a short tone burst carrying no bits: the receiver does not
 * decode it, it correlates for it.  That is the whole idea.  A coded frame has
 * to be acquired and then decoded, and both can fail; a known tone sequence
 * only has to be recognised, which it can be several dB further down and --
 * more importantly -- without the delivery ceiling a coded burst hits under
 * fading.
 *
 * Measured on the project's Watterson channel against the mode it replaces
 * (docs/ACK-CHANNEL.md), at 0.64 s against DATAC16's 3.30 s:
 *
 *              50% point (SNR3k)          reaches 100%
 *              AWGN    MPG     MPD
 *   pattern    -13.9   -14.0   -13.2      yes, by -1.7 dB (MPG)
 *   DATAC16      --    -10.9    -8.0      no: floors near 77%
 *
 * The ceiling is the reason this exists.  One ACK in five lost on a GOOD link
 * is a retransmission on every fifth frame, permanently, at the top of the
 * ladder where throughput is supposed to be won.
 *
 * The cost is capacity: this channel carries ACK, ACK+TURN and HAIL -- about
 * two bits.  It cannot carry a sequence number or a selective-repeat bitmap.
 * That is an exact fit for delivery-driven stop-and-wait, where one frame is
 * outstanding so a heard ACK is unambiguous, and a hard stop for a windowed
 * data plane.  Choose deliberately.
 *
 * Copyright (C) 2022-2024 Fadi Jerji (original tone geometry, C++)
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef MERCURY_PATTERN_ACK_H
#define MERCURY_PATTERN_ACK_H

#include <stdint.h>

/* What a pattern says.  These are the only things it CAN say. */
typedef enum {
    PATTERN_ACK   = 0,   /* frame received                       */
    PATTERN_BREAK = 1    /* frame received, and I want the turn   */
} pattern_kind_t;

/* Number of pattern symbols (airtime / buffer sizing). */
int pattern_ack_nsymb(void);

/* Max int16 passband samples a pattern TX produces (buffer sizing). */
int pattern_ack_max_tx_samples(void);

/* Generate the pattern as int16 passband.  Returns the sample count written,
 * or 0 on allocation failure.  `out` must hold pattern_ack_max_tx_samples(). */
int pattern_ack_tx(int16_t *out, pattern_kind_t kind);

/* Look for a pattern in an int16 passband chunk.  Returns 1 on a match and
 * sets *is_break (1 = ACK+TURN, 0 = plain ACK); 0 if none.
 *
 * NOT cheap: measured at 3.5k samp/s against 8k arriving, i.e. roughly 44% of
 * a real-time RX budget, because it slides a matched filter over every
 * candidate start.  Run it inside bounded windows where a pattern is actually
 * due, never continuously -- doing the latter is what once cost the RX loop
 * enough time to miss the connect-critical control frame. */
int pattern_ack_detect(const int16_t *pb, int n, int *is_break);

#endif /* MERCURY_PATTERN_ACK_H */
