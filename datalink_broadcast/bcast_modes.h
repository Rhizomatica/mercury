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

/* Joint configuration+payload frame, as hermes-broadcast's broadcast_daemon
 * builds it.  EVERY frame is self-describing:
 *
 *   [0]        header: packet type in the top 3 bits, session id in the low 5
 *   [1..8]     config body -- 5-byte reduced common OTI + 3-byte reduced scheme
 *   [9..11]    reduced RaptorQ tag -- sbn + 16-bit ESI little-endian
 *   [12..]     the symbol
 *
 * The older split format (transmitter.c) sent the configuration as its own
 * periodic frame and spent only 4 bytes per payload frame.  The joint format
 * costs 8 more bytes per frame and is still the right trade for broadcast:
 * there is no return path, so a receiver tunes in at an arbitrary point, and
 * with the split format it can decode nothing until the next configuration
 * frame happens to arrive.  Here it can start on the very first frame it hears. */
#define BCAST_CONFIG_BODY_SIZE 8
#define BCAST_TAG_BODY_SIZE    3
#define BCAST_FRAME_OVERHEAD   (1 + BCAST_CONFIG_BODY_SIZE + BCAST_TAG_BODY_SIZE)

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
