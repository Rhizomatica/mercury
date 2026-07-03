/* libFuzzer target: KISS deframer.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * kiss_read() is a stateful, byte-at-a-time deframer backed by file-scope
 * globals. We reset that state at the start of every input, then feed each
 * fuzz byte through it. The frame buffer must be at least MAX_PAYLOAD (756)
 * bytes; we oversize it so a length bug shows up as an ASan overflow rather
 * than corrupting the harness's own stack silently.
 */
#include <stddef.h>
#include <stdint.h>

#include "kiss.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    uint8_t frame[MAX_PAYLOAD + 64];

    kiss_reset_state();
    for (size_t i = 0; i < size; i++)
        (void)kiss_read(data[i], frame);

    return 0;
}
