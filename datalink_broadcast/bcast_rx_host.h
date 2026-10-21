/* The host's broadcast receiver (bcast_rx_host.c)
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef BCAST_RX_HOST_H_
#define BCAST_RX_HOST_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef void (*bcast_host_object_fn)(const uint8_t *data, size_t len, const char *name,
                                     bool sealed_ok, void *user);
typedef void (*bcast_host_frame_fn)(const uint8_t *frame, size_t len, int mode, float snr,
                                    void *user);

/* Start (or restart) handing the host what the broadcast plane hears; both
 * NULL stops it.  The callbacks run on a thread of this module's.  0, or -1
 * when the receiver could not start. */
int bcast_rx_host_set(bcast_host_object_fn on_object, bcast_host_frame_fn on_frame, void *user);

#endif /* BCAST_RX_HOST_H_ */
