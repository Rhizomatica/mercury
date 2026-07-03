/* tests/sim/sim_translate.h
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Rhizomatica */
#ifndef SIM_TRANSLATE_H
#define SIM_TRANSLATE_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "arq_fsm.h"

/* Decode a frame emitted by send_tx_frame into an arq_event_t for the receiver.
 * receiver_call is used for CALL/ACCEPT DST validation. Returns true if the
 * frame produced a dispatchable event. */
bool sim_translate_frame(const uint8_t *frame, size_t frame_size, float rx_snr,
                         const char *receiver_call, arq_event_t *out_ev);

#endif
