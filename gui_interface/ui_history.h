/* HERMES Modem
 *
 * Copyright (C) 2026 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3, or (at your option)
 * any later version.
 *
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef UI_HISTORY_H_
#define UI_HISTORY_H_

#include <stddef.h>

/**
 * @brief Build a `{"type":"history","messages":[...]}` frame from a
 *        newline-delimited JSONL snapshot.
 *
 * The snapshot is the buffer returned by msg_store_snapshot(): `count` lines,
 * each already a complete JSON object, separated by newlines, `snap_len` bytes
 * in total (excluding any NUL).  The lines are joined with commas inside a
 * `messages` array.
 *
 * @param snap      Snapshot buffer (newline-delimited JSONL).
 * @param count     Number of lines in the snapshot.
 * @param snap_len  Total length of the snapshot in bytes.
 * @return A malloc'd, NUL-terminated string the caller must free(), or NULL on
 *         allocation failure.
 */
char *ui_history_frame_build(const char *snap, size_t count, size_t snap_len);

#endif /* UI_HISTORY_H_ */
