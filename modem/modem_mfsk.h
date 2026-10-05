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

/* Bind the ACK and BREAK patterns to a session: from now on mfsk_pattern_tx()
 * sends, and the detectors look for, the session's lists (mfsk_session_patterns
 * of key), so another station's patterns -- every station's were the same --
 * are not taken for the peer's.  Key 0: the global lists.  Returns whether
 * session lists are in use. */
bool mfsk_pattern_set_session(uint32_t key);
/* The same with explicit lists (ack_pattern_len tones each; NULL: global). */
void mfsk_pattern_set_lists(const int *ack, const int *brk);

/* Detect a pattern ACK in an int16 passband chunk.  Returns 1 on a match and
 * sets *is_break (1 = break/ACK+TURN, 0 = plain ACK); 0 if none. */
int mfsk_pattern_detect(const int16_t *pb, int n, int *is_break);

/* ----------------------------------------------------------------------
 * Streaming detection, for a pattern arriving as a stream of chunks.
 *
 * mfsk_pattern_detect() scores every candidate start of a buffer with an FFT
 * per symbol under it, so its cost grows with the square of the window; run on
 * every chunk it once cost 4.5 s of CPU per second of audio, and then, paced
 * to a scan per burst, still about 7 % of real time, so it ran only inside
 * WAIT_ACK windows.  The window now downmixes and filters each sample once and
 * feeds mfsk_stream_det (mfsk_sync.h), which does one FFT per step and scores
 * by lookup: the same scores, found as the audio arrives.
 *
 * A burst is reported once, as it ends (a few steps after its last symbol).
 * ---------------------------------------------------------------------- */
struct mfsk_stream_det;
typedef struct {
    struct mfsk_stream_det *det;   /* created on the first push */
    double  hist[64];              /* the low-pass filter's last inputs, signed by the downmix */
    int     hist_pos;
    long long n;                   /* samples pushed since the reset */
    unsigned gen;                  /* the pattern lists it scores (mfsk_pattern_set_session) */
    long long nav_start;           /* the last NAV header reported, and its score */
    int     nav_score;
} mfsk_pattern_window_t;

/* A pattern the window found: ACK, BREAK, or a NAV header of class k
 * (MFSK_PAT_NAV(k)); `start` is its first sample, counted from the window's
 * last reset (or creation), so a caller that knows the sample count of its
 * own last chunk knows how long ago it began. */
#define MFSK_PAT_ACK     0
#define MFSK_PAT_BREAK   1
#define MFSK_PAT_NAV(k)  (2 + (k))
typedef struct {
    int       kind;
    long long start;
    int       score;
} mfsk_pattern_ev_t;
/* Push `n` samples; up to maxev events are written.  Returns how many. */
int  mfsk_pattern_window_events(mfsk_pattern_window_t *w, const int16_t *pcm, int n,
                                mfsk_pattern_ev_t *ev, int maxev);
/* Samples pushed since the reset (the clock `start` runs on). */
long long mfsk_pattern_window_samples(const mfsk_pattern_window_t *w);

/* The NAV header of class k (mfsk.h), int16 passband; returns the sample
 * count (mfsk_pattern_max_tx_samples()). */
int  mfsk_nav_tx(int16_t *out, int cls);

/* Push `n` samples; returns 1 when an ACK or BREAK is found (is_break set),
 * ignoring NAV headers.  Never fails hard: without memory it cannot detect. */
int  mfsk_pattern_window_push(mfsk_pattern_window_t *w, const int16_t *pcm, int n,
                              int *is_break);

/* Discard buffered audio.  Call when a NEW pattern becomes due: the window
 * holds over a second, and a burst left unmatched by the previous exchange --
 * the gate closed first, or the scan had not come round -- would otherwise be
 * found by the next scan and reported as THIS frame's ACK, advancing the
 * sender past a frame the peer never received. */
void mfsk_pattern_window_reset(mfsk_pattern_window_t *w);

/* Free the window's buffer (safe on a zeroed or already-freed window). */
void mfsk_pattern_window_free(mfsk_pattern_window_t *w);

#endif /* MERCURY_MODEM_MFSK_H */
