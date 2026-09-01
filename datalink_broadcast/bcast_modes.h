/* HERMES broadcast wire constants — shared with hermes-broadcast.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Trimmed from hermes-broadcast's mercury_modes.h.  Two deliberate changes:
 * the frame-size table is `static const` (the original is a bare global in a
 * header, which multiply-defines as soon as two translation units include it),
 * and only the pieces Mercury actually needs are kept.
 *
 * These values are WIRE FORMAT.  Changing one breaks interoperability with
 * every hermes-broadcast receiver in the field.
 */
#ifndef BCAST_MODES_H_
#define BCAST_MODES_H_

#include <stdint.h>

/* Configuration packet: 1 header byte + 5-byte reduced common OTI +
 * 3-byte reduced scheme-specific OTI. */
#define BCAST_CONFIG_PACKET_SIZE 9

/* Per-payload-frame overhead: 1 header byte + 3-byte reduced RaptorQ tag. */
#define BCAST_RQ_HEADER_SIZE 4

/* The reduced tag carries a 16-bit ESI, so the carousel wraps at 65535. */
#define BCAST_MAX_ESI ((1 << 16) - 1)

/* Frame header byte: 3-bit packet type, 5-bit extension field. */
#define BCAST_PACKET_TYPE_SHIFT 5
#define BCAST_PACKET_TYPE_MASK  0x07
#define BCAST_FRAME_EXT_MASK    0x1F

#define BCAST_PACKET_RQ_CONFIG  0x03
#define BCAST_PACKET_RQ_PAYLOAD 0x04

#define BCAST_MODE_MAX 10   /* modes 0..10 */

/* payload_bytes_per_modem_frame, in the order `mercury -l` reports:
 * DATAC1, DATAC3, DATAC0, DATAC4, DATAC13, DATAC14,
 * FSK_LDPC, DATAC15, DATAC16, DATAC17, QAM16C2. */
static const uint32_t bcast_frame_size[BCAST_MODE_MAX + 1] = {
    510, 126, 14, 54, 14, 3, 30, 30, 14, 1180, 1213
};

static inline void bcast_write_frame_header(uint8_t *frame, uint8_t packet_type,
                                            uint8_t extension)
{
    if (!frame)
        return;
    frame[0] = (uint8_t)(((packet_type & BCAST_PACKET_TYPE_MASK) << BCAST_PACKET_TYPE_SHIFT) |
                         (extension & BCAST_FRAME_EXT_MASK));
}

#endif /* BCAST_MODES_H_ */
