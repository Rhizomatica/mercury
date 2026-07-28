/* 24-bit packed PCM conversion (FFAUDIO_F_INT24 / SND_PCM_FORMAT_S24_3LE).
 *
 * Three bytes per sample, little-endian on the wire regardless of host order,
 * carrying a signed 24-bit value.  This is the 24-bit format ALSA and
 * PulseAudio expose; it is NOT FFAUDIO_F_INT24_4, which puts a 24-bit sample
 * in a 4-byte container and is what WASAPI negotiates on Windows.  Mercury
 * handled only the latter, so 24-bit cards worked on Windows and were
 * rejected on Linux.
 *
 * The modem rings hold int32 full scale, so the pair is a scale by 256:
 * reading multiplies, writing divides.  They are exact inverses apart from
 * the low 8 bits, which a 24-bit device cannot represent anyway.
 *
 * Kept in a header (like sock_wire.h) so the conversion is unit-testable
 * without a sound card — see tests/audioio/test_pcm24.c, which pins the byte
 * layout and the round trip.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef AUDIOIO_PCM24_H
#define AUDIOIO_PCM24_H

#include <stdint.h>
#include <stddef.h>

#define PCM24_BYTES 3

/* One packed 24-bit LE sample -> int32 full scale.
 * Assembled through an unsigned value and sign-extended explicitly: shifting
 * a negative int is implementation-defined, and the byte order must not
 * depend on the host's. */
static inline int32_t pcm24_rd_le(const uint8_t *p)
{
    uint32_t u = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
    if (u & 0x800000u)
        u |= 0xFF000000u;              /* sign-extend bit 23 */
    return (int32_t)u * 256;           /* 24-bit -> int32 full scale */
}

/* int32 full scale -> one packed 24-bit LE sample.
 * Division, not >>8: it is defined for negative values (truncates toward
 * zero), and the byte extraction goes through an unsigned copy so no
 * implementation-defined right shift of a negative is involved. */
static inline void pcm24_wr_le(uint8_t *p, int32_t s)
{
    uint32_t v = (uint32_t)(s / 256);
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
    p[2] = (uint8_t)((v >> 16) & 0xffu);
}

#endif /* AUDIOIO_PCM24_H */
