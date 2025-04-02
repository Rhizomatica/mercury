#include "datalink_layer/fsm.h"
#include <stdlib.h>

void Fsm_Init(fsm_handle* fsm, fsm_state initial_state) {
    fsm->current = initial_state;
    pthread_mutex_init(&fsm->lock, NULL);
}

void Fsm_Dispatch(fsm_handle* fsm, int event) {
    pthread_mutex_lock(&fsm->lock);
    if (fsm->current) {
        fsm->current(event);  // Execute current state
    }
    pthread_mutex_unlock(&fsm->lock);
}

void Fsm_Destroy(fsm_handle* fsm)
{
    pthread_mutex_destroy(&fsm->lock);
}
