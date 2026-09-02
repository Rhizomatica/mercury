/* Mercury Modem — persistent message store
 *
 * Stores the chat messages exchanged over the ARQ and broadcast links so they
 * survive application restarts.  Messages are appended to a JSONL file and
 * kept in a bounded in-memory ring for retrieval (e.g. the HISTORY TNC command).
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef MESSAGE_STORE_H_
#define MESSAGE_STORE_H_

#include <stddef.h>
#include <stdint.h>

/* Message plane — which link the message travelled on. */
#define MSG_PLANE_ARQ   "arq"
#define MSG_PLANE_BCAST "bcast"

/* Message direction. */
#define MSG_DIR_RX      "rx"
#define MSG_DIR_TX      "tx"

/* Default in-memory capacity (also caps how many recent messages are returned
 * by msg_store_get()/HISTORY).  Overridden by [store] max_messages. */
#define MSG_STORE_DEFAULT_MAX_LINES 500

/**
 * @brief Initialise the message store.
 *
 * Opens (or creates) the JSONL file and loads existing messages into the
 * in-memory ring.  Calling again after a previous successful init first
 * shuts the store down.
 *
 * @param path      JSONL file path.  NULL or "" selects the platform default
 *                  data-directory path (messages.jsonl).
 * @param max_lines In-memory ring capacity (<= 0 uses the default).
 * @return 0 on success, -1 on failure (store is left disabled).
 */
int  msg_store_init(const char *path, int max_lines);

/**
 * @brief Flush and close the store.  Safe to call when not initialised.
 */
void msg_store_shutdown(void);

/**
 * @brief Feed raw bytes from one direction of one plane.
 *
 * Bytes are buffered and split on newlines; each complete line that is
 * printable text is stored.  Binary data (e.g. file transfers or AX.25
 * frames) contains control bytes and is filtered out.
 *
 * @param plane MSG_PLANE_ARQ or MSG_PLANE_BCAST.
 * @param dir   MSG_DIR_RX or MSG_DIR_TX.
 * @param peer  Peer callsign (may be NULL or "" if unknown).
 * @param data  Raw bytes (need not be NUL-terminated).
 * @param len   Number of bytes.
 */
void msg_store_feed(const char *plane, const char *dir, const char *peer,
                    const uint8_t *data, size_t len);

/**
 * @brief Store one complete text message directly (no newline splitting).
 *
 * The text is stored verbatim (trailing newline stripped) if it is printable
 * and non-empty.
 */
void msg_store_append(const char *plane, const char *dir, const char *peer,
                      const char *text);

/**
 * @brief Number of messages currently in the in-memory ring.
 */
size_t msg_store_count(void);

/**
 * @brief Copy one stored message out as a JSONL line (no trailing newline).
 *
 * Messages are indexed oldest-first: index 0 is the oldest, count-1 the
 * newest.  The JSON is compact (single line).
 *
 * @param index   Message index (0 .. msg_store_count()-1).
 * @param buf     Output buffer.
 * @param buf_cap Output buffer capacity (including room for a NUL).
 * @return Bytes written (excluding NUL), or 0 if index is out of range.
 */
size_t msg_store_get(size_t index, char *buf, size_t buf_cap);

#endif /* MESSAGE_STORE_H_ */
