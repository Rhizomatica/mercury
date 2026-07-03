/* libFuzzer target: modem framer-byte parser.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * parse_frame_header() inspects data[0] to classify a decoded modem frame.
 * It must reject size 0 and never read past frame_size for any input.
 */
#include <stddef.h>
#include <stdint.h>

#include "framer.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    uint8_t ext = 0;
    /* frame_size is uint32_t on the API; clamp so a >4 GiB fuzz input can't
     * wrap the argument (libFuzzer inputs are far smaller in practice). */
    uint32_t n = (size > 0xffffffffu) ? 0xffffffffu : (uint32_t)size;
    (void)parse_frame_header(data, n, &ext);
    return 0;
}
