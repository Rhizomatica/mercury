/*
 * Mercury: A configurable open-source software-defined modem.
 * Copyright (C) 2025 Rhizomatica
 * Author: Rafael Diniz
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, version 3 of the
 * License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 */

#include "datalink_layer/fsm.h"
#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>

const char *fsm_event_names[] = {
    "EV_CLIENT_CONNECT",
    "EV_CLIENT_DISCONNECT",
    "EV_START_LISTEN",
    "EV_STOP_LISTEN",
    "EV_LINK_CALL_REMOTE",
    "EV_LINK_INCOMING_CALL",
    "EV_LINK_DISCONNECT",
    "EV_LINK_ESTABLISHMENT_TIMEOUT",
    "EV_LINK_ESTABLISHED"
};

// Initialize the FSM
void fsm_init(fsm_handle* fsm, fsm_state initial_state)
{
    printf("Initializing FSM\n");
    
    if (!fsm)
        return;

    fsm->current = initial_state;
    pthread_mutex_init(&fsm->lock, NULL);
}

// Dispatch an event (thread-safe)
void fsm_dispatch(fsm_handle* fsm, int event)
{
    if (!fsm)
        return;

    printf("Dispatching event %s\n", fsm_event_names[event]);
    
    pthread_mutex_lock(&fsm->lock);
    if (fsm->current)
        fsm->current(event);  // Execute current state

    pthread_mutex_unlock(&fsm->lock);
}

// Clean up resources
void fsm_destroy(fsm_handle* fsm)
{
    if (!fsm)
        return;

    printf("Destroying FSM\n");
    
    pthread_mutex_destroy(&fsm->lock);
    fsm->current = NULL;
}
