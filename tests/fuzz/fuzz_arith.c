/* libFuzzer target: arithmetic decoder.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * arithmetic_decode() reads an encoded bitstream and writes decoded text into
 * a caller buffer. It must bound both its input reads (BitReader) and its
 * output writes (max_output) for any malformed stream. init_model() sets up
 * the static frequency table and must be called once before decoding; note
 * the decode path does NOT touch the encoder's exit(1) at arith.c:63.
 */
#include <stddef.h>
#include <stdint.h>

/* Declared in arith.c; no public header exposes these, so declare locally. */
void init_model(void);
int  arithmetic_decode(uint8_t *input, int max_len, char *output, int max_output);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static int model_ready = 0;
    if (!model_ready) { init_model(); model_ready = 1; }

    /* max_len is int on the API; clamp so a large fuzz input can't go negative. */
    int in_len = (size > 0x7fffffffu) ? 0x7fffffff : (int)size;

    char out[4096];
    (void)arithmetic_decode((uint8_t *)data, in_len, out, (int)sizeof(out));
    return 0;
}
