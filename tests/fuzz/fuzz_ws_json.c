/* libFuzzer target: WebSocket JSON command parser.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * ws_json_parse_command() reads whatever a WebSocket client sends to the UI
 * port, which is unauthenticated and bound to 0.0.0.0, so it parses hostile
 * input by construction.  It is a hand-rolled scanner over a
 * length-delimited buffer with no NUL guarantee -- exactly the shape that
 * over-reads.
 *
 * This target was deferred for a long time because the helpers were static
 * inside a translation unit that included a ~990 KB HTTP/WS amalgamation.
 * That library is gone and the helpers now live in ws_json.c, which depends
 * on nothing but libc.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "ws_json.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    ws_command_t cmd;
    char *buf;

    /* Copy into an exactly-sized heap block so ASan traps a read even one
     * byte past the input -- the bug class that matters here.  A stack array
     * would hide it behind padding. */
    buf = (char *) malloc(size ? size : 1);
    if (buf == NULL)
        return 0;
    if (size > 0)
        memcpy(buf, data, size);

    if (ws_json_parse_command(buf, size, &cmd) == 0)
    {
        /* Every field the caller goes on to use must be NUL-terminated
         * within its own array. */
        if (memchr(cmd.command, '\0', sizeof(cmd.command)) == NULL)
            abort();
        if (memchr(cmd.value, '\0', sizeof(cmd.value)) == NULL)
            abort();
        if (memchr(cmd.value7, '\0', sizeof(cmd.value7)) == NULL)
            abort();
    }

    /* The sanitiser rewrites a buffer in place and must stay inside it. */
    ws_json_sanitise_utf8(buf, size);

    free(buf);
    return 0;
}
