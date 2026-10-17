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


/* Symbol size, decoupled from the modem frame.
 *
 * The obvious sizing is one symbol per frame, T = frame - overhead, and that is
 * what this used to do.  It is the most efficient possible packing and it has
 * one fatal property: T is then a function of the MODE, so RaptorQ symbols
 * collected at one mode are worthless at another.  Changing mode mid-transfer
 * throws away everything the receiver has, and interleaving two modes in one
 * carousel is impossible.
 *
 * Fixing T instead makes a symbol mode-independent: any mode can carry symbols
 * for the same source block, a mode change costs nothing, and one transmission
 * can serve a fast audience and a fringe audience at the same time -- each
 * decoding whatever it can, all of it counting toward the same object.
 *
 * 41 bytes is the largest T that still fits a DATAC4 frame (54 - 9 fixed - 3
 * tag), so DATAC4 is the most robust rung that can carry one whole symbol.
 * Going lower to admit DATAC15 (30 B) costs 7 points of efficiency at the fast
 * end to serve a rung carrying 17 B a frame; DATAC16 can never participate at
 * all (14 B frame, 5 left after the fixed part).
 *
 * The symbols packed into one frame are CONSECUTIVE ESIs of ONE source block,
 * so the frame's single reduced tag describes all of them: the receiver reads
 * the base sbn/ESI and derives ESI+1, ESI+2, ... for the rest.  Tagging each
 * symbol separately would cost 3 bytes apiece -- 6.8 points at QAM16C2 -- to
 * buy a per-symbol block spread that a fountain code does not need, since
 * every block has to reach K+e either way.
 *
 * The cost of mode-independence is then 1 point: 98.0% payload efficiency at
 * QAM16C2 against 99.0% for one-symbol-per-frame -- against the 100% extra a
 * second carousel for the fringe audience would cost. */
#define BCAST_SYMBOL_SIZE_MIN  41

/* How many whole symbols of `T` a frame of `frame_size` carries.
 *
 * Both ends compute this the same way, and the RECEIVER computes it from the
 * length of the frame it just decoded rather than from any configured mode.
 * That is what an interleaved carousel would need -- frames of several modes
 * on the air at once, the receiver taking whichever it managed to decode
 * without being told which was sent.
 *
 * NOT YET REACHABLE, deliberately: this is the fixed-single-mode step.
 * Fixing T is what makes interleaving POSSIBLE later -- symbols become
 * mode-independent, so symbols a receiver collects all count toward the same
 * object no matter which mode carried them.
 *
 * The receiving machinery for it already exists: Mercury runs a DUAL
 * receiver.  modem.c tees each audio chunk into two independent rx workers,
 * the control plane and the user plane, each decoding on its own mode, and
 * BOTH deliver through process_received_frame().  So a mixed carousel does
 * not need a new decoder -- it needs the two workers pointed at the two
 * interleaved modes, and then the two size gates relaxed to admit both frame
 * sizes rather than one: modem.c's single broadcast_frame_size, and
 * bcast_file_rx_frame()'s len != rx->frame_size.
 *
 * Note the spare capacity that makes this cheap: while ARQ is disconnected
 * the control-plane worker is pinned to DATAC16 (arq_modem_preferred_rx_mode()
 * always returns the control mode) and is decoding nothing useful for
 * broadcast.  That is a whole idle decoder available to the fringe rung. */
static inline unsigned bcast_syms_per_frame(size_t frame_size, size_t T)
{
    if (T == 0 || frame_size <= BCAST_FRAME_OVERHEAD)
        return 0;
    return (unsigned)((frame_size - BCAST_FRAME_OVERHEAD) / T);
}

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

/* Names as `mercury -l` and hermes-broadcast use them, in the same order as
 * bcast_frame_size so the two cannot drift apart. */
static const char *const bcast_mode_name[BCAST_MODE_MAX + 1] = {
    "DATAC1", "DATAC3", "DATAC0", "DATAC4", "DATAC13", "DATAC14",
    "FSK_LDPC", "DATAC15", "DATAC16", "DATAC17", "QAM16C2"
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
