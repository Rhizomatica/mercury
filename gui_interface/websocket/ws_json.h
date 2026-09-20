/* Flat-JSON helpers for the Mercury UI command protocol
 *
 * Copyright (C) 2026 Rhizomatica
 * Authors: Pedro Messetti <pedromessetti.rhizomatica@gmail.com>
 *          Rafael Diniz <rafael@riseup.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Split out of mercury_websocket.c so these can be tested and fuzzed on their
 * own: the parser is reachable by anything that can open the UI port, and the
 * fuzz target for it was blocked while it lived next to a 29k-line library
 * (see tests/fuzz/fuzz_ws_json.c).  This unit deliberately depends on nothing
 * but libc.
 */

#ifndef WS_JSON_H_
#define WS_JSON_H_

#include <stddef.h>

/* ---- Incoming command from the UI ---- */
typedef struct {
    char command[64];   /* e.g. "set_audio_config", "set_ptt_config" */
    char value[256];    /* primary value (e.g. device id, channel name) */
    char value2[256];   /* optional second value (e.g. device path) */
    char value3[256];   /* optional third value (e.g. input channel) */
    char value4[256];   /* optional fourth value (e.g. Hamlib speed) */
    char value5[256];   /* optional fifth value (e.g. serial PTT line) */
    char value6[256];   /* optional sixth value (e.g. serial PTT inversion) */
    char value7[256];   /* optional seventh value (e.g. CM108 GPIO pin) */
} ws_command_t;

/* Find the value for `key` in a flat JSON object.  Strings come back
 * unquoted, numbers and literals raw.  Returns 1 when found, 0 otherwise;
 * `out` is always NUL-terminated when 1 is returned. */
int ws_json_find_key(const char *json, size_t json_len, const char *key,
                     char *out, size_t out_sz);

/* Parse {"command":"...","value":"...",...,"value7":"..."}.
 * "command" is mandatory.  Returns 0 on success, -1 when it is missing. */
int ws_json_parse_command(const char *json, size_t len, ws_command_t *cmd);

/* Replace bytes that are not valid UTF-8, and control characters below 0x20,
 * with '?'.  WebSocket TEXT frames must be valid UTF-8, and an unescaped
 * control character is illegal inside a JSON string, which the browser
 * reports as "Invalid control character". */
void ws_json_sanitise_utf8(char *s, size_t len);

#endif /* WS_JSON_H_ */
