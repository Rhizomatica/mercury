/* The host's broadcast carousel, as it reports itself
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef BCAST_STATUS_H_
#define BCAST_STATUS_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The host's broadcast carousel (datalink_broadcast/bcast_carousel.c): one at
 * a time, and what it is doing, for anyone who asks -- Mercury's status, the
 * MODE command (BUSY while one sends), the host program.  Kept here, in every
 * build, though only the host and GUI builds can run one. */
typedef enum {
    BCAST_CAR_IDLE = 0,       /* none sent yet                                */
    BCAST_CAR_SENDING,
    BCAST_CAR_STOPPED_BY_ARQ, /* an ARQ session started: ARQ comes first      */
    BCAST_CAR_STOPPED,        /* the host stopped it                          */
    BCAST_CAR_DONE,           /* every cycle sent                             */
    BCAST_CAR_ERROR
} bcast_car_state_t;

typedef struct {
    bcast_car_state_t state;
    int      mode;            /* the listen-mode index it sends at            */
    int      cycle;           /* the cycle now on, 1-based                    */
    int      cycles_total;    /* 0: until stopped                             */
    uint64_t frames_sent;
    size_t   bytes;           /* the file's                                   */
    char     name[64];
} bcast_car_status_t;

void bcast_carousel_publish(const bcast_car_status_t *st);
void bcast_carousel_get(bcast_car_status_t *out);
bool bcast_carousel_sending(void);
const char *bcast_car_state_name(bcast_car_state_t st);

#endif /* BCAST_STATUS_H_ */
