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

/* ---- The bundle ------------------------------------------------------------
 *
 * RaptorQ moves an opaque blob, so the filename has to travel inside it.  The
 * layout is mercury-connector's (spool.c), which already solved this:
 *
 *     [0..3]   uint32 total = strlen(basename) + 1 + file bytes
 *     [4..]    basename, terminated by '
' rather than NUL
 *     [...]    file contents
 *
 * Only the BASENAME is sent -- never a path -- so a hostile or careless sender
 * cannot make a receiver write outside its download directory.
 *
 * The size field is written explicitly little-endian.  mercury-connector writes
 * a native uint32, which is the same bytes on every machine this runs on today
 * and wrong on a big-endian one; being explicit costs nothing and removes the
 * trap.
 *
 * hermes-broadcast's broadcast_daemon has no filename mechanism at all -- it
 * names what it receives broadcast_<timestamp>.bin -- so a daemon receiving
 * this gets the bundle verbatim under a timestamp name, and a bundle-aware
 * receiver recovers the real name.  Nothing breaks either way.
 */

/** Longest basename we will transmit or accept. */
#define BCAST_BUNDLE_NAME_MAX 255

/** Bytes of overhead a bundle adds to the file: the size field and "name
". */
#define BCAST_BUNDLE_OVERHEAD(namelen) (4u + (unsigned)(namelen) + 1u)

/**
 * Build the bundle for a file.  Caller frees the returned buffer.
 * @return the buffer, or NULL with a reason in @p err.
 */
uint8_t *bcast_bundle_build(const char *path, size_t *out_len,
                            char *err, size_t errlen);

/**
 * Parse a bundle.  @p payload points INTO @p buf; nothing is copied.
 * @return 0 on success, -1 if the bundle is malformed.
 */
int bcast_bundle_parse(const uint8_t *buf, size_t len,
                       char *name, size_t namelen,
                       const uint8_t **payload, size_t *payload_len);

typedef struct bcast_file_tx bcast_file_tx_t;

/**
 * Open a file for broadcast.
 *
 * @param path    file to transmit; the BUNDLE (file + name + header) must be
 *                <= BCAST_FILE_MAX_BYTES, and the file must be non-empty
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

/* ---- Receiving --------------------------------------------------------------
 *
 * Feed every broadcast frame in; the receiver picks out the ones it can use.
 * It learns the transfer parameters from the first usable frame -- every frame
 * carries them -- so it can join a carousel already in progress.
 *
 * A new session id means a different file: the receiver restarts rather than
 * mixing symbols from two files into one decode, which would never converge. */

typedef struct bcast_file_rx bcast_file_rx_t;

/**
 * @param mode  Mercury mode index; frames of any other size are ignored.
 * @param dir   directory received files are written into.
 */
bcast_file_rx_t *bcast_file_rx_open(int mode, const char *dir,
                                    char *err, size_t errlen);

/** Result of feeding one frame. */
typedef enum {
    BCAST_RX_IGNORED = 0,  /* not for us (wrong size, wrong type)      */
    BCAST_RX_PROGRESS,     /* accepted, still incomplete               */
    BCAST_RX_COMPLETE,     /* file finished and written                */
    BCAST_RX_ERROR         /* malformed beyond recovery; see the error */
} bcast_rx_result_t;

bcast_rx_result_t bcast_file_rx_frame(bcast_file_rx_t *rx,
                                      const uint8_t *frame, size_t len);

/** Path of the last completed file, or "" if none yet. */
const char *bcast_file_rx_last_path(const bcast_file_rx_t *rx);
/** Name the sender gave the last completed file. */
const char *bcast_file_rx_last_name(const bcast_file_rx_t *rx);
/** Symbols accepted into the current decode, and the total the file needs. */
void bcast_file_rx_stats(const bcast_file_rx_t *rx, uint64_t *symbols,
                         size_t *expect_bytes);
const char *bcast_file_rx_error(const bcast_file_rx_t *rx);

void bcast_file_rx_close(bcast_file_rx_t *rx);

/** Payload bytes per modem frame for a mode, or 0 if the mode is out of range. */
int bcast_file_mode_frame_size(int mode);

/** Whether a mode can carry broadcast at all (DATAC14's 3 bytes cannot). */
int bcast_file_mode_usable(int mode);

#endif /* BCAST_FILE_H_ */
