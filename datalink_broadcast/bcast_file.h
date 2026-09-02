/* HERMES Modem — broadcast file transmission (RaptorQ carousel)
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Wire-compatible with hermes-broadcast's `broadcast_daemon`, so a station
 * running this and a station running that interoperate.  Every frame is
 * self-describing -- header, reduced OTI, reduced tag, symbol -- which is what
 * lets a receiver with no return path start decoding on whichever frame it
 * happens to hear first.  See bcast_modes.h for the exact layout.
 *
 * Broadcast has no return path, so there is no acknowledgement and no
 * retransmission: the sender simply repeats the file as a RaptorQ carousel and
 * a receiver decodes as soon as it has collected enough distinct symbols, from
 * wherever in the stream it happened to start listening.  "Cycles" is how many
 * times to go round.
 *
 * This produces FRAMES ONLY.  It does no I/O: the caller sends each frame over
 * whatever transport it already has (the UI sends them over the same broadcast
 * TCP/KISS socket its chat uses).  That keeps the codec testable without a
 * modem, a socket, or a radio.
 */
#ifndef BCAST_FILE_H_
#define BCAST_FILE_H_

#include <stddef.h>
#include <stdint.h>

/* A floppy disk.  The cap exists because the whole file is held in memory and
 * because a carousel over HF is slow: at DATAC1's 510-byte frames even this
 * takes a long while, and anything larger is a mistake the UI should refuse
 * rather than discover hours in. */
#define BCAST_FILE_MAX_BYTES (1440u * 1024u)

/* Longest frame any supported mode carries (QAM16C2 = 1213). */
#define BCAST_FILE_MAX_FRAME 1213

typedef struct bcast_file_tx bcast_file_tx_t;

/**
 * Open a file for broadcast.
 *
 * @param path    file to transmit; must be <= BCAST_FILE_MAX_BYTES and non-empty
 * @param mode    Mercury mode index 0..10 (as `mercury -l` reports)
 * @param cycles  number of carousel passes, or 0 to repeat until stopped
 * @param session_id  1..31 identifying this file, or 0 to pick one at random.
 *                    Carried in every frame so a receiver can tell a new file
 *                    from the one it just finished.
 * @param err     receives a human-readable reason on failure; may be NULL
 * @return the handle, or NULL on failure
 */
bcast_file_tx_t *bcast_file_tx_open(const char *path, int mode, int cycles,
                                    int session_id, char *err, size_t errlen);

/**
 * Produce the next frame to transmit.
 *
 * @return frame length in bytes, 0 when the run is complete, or -1 on error.
 *
 * A cycles == 0 run is endless in practice but not in principle: the reduced
 * tag carries only a 16-bit ESI, so it also ends once a block's ESI space is
 * exhausted.  Continuing past that would wrap and re-send symbols the receiver
 * already has, which looks like progress and is not.
 */
int bcast_file_tx_next(bcast_file_tx_t *tx, uint8_t *buf, size_t buflen);

/** Frame size of the chosen mode, i.e. the length every frame will be. */
int bcast_file_tx_frame_size(const bcast_file_tx_t *tx);

/** Progress. Any out parameter may be NULL. cycles_total is 0 for endless. */
void bcast_file_tx_stats(const bcast_file_tx_t *tx, int *cycle_now,
                         int *cycles_total, uint64_t *frames_sent);

/** Bytes of the source file, and the number of RaptorQ blocks it needed. */
void bcast_file_tx_source(const bcast_file_tx_t *tx, size_t *file_bytes, int *blocks);

void bcast_file_tx_close(bcast_file_tx_t *tx);

/** Payload bytes per modem frame for a mode, or 0 if the mode is out of range. */
int bcast_file_mode_frame_size(int mode);

/** Whether a mode can carry broadcast at all (DATAC14's 3 bytes cannot). */
int bcast_file_mode_usable(int mode);

#endif /* BCAST_FILE_H_ */
