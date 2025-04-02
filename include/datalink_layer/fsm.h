#ifndef FSM_H
#define FSM_H

#include <pthread.h>

// State function pointer type
typedef void (*fsm_state)(int event);

// Thread-safe FSM structure
typedef struct {
    fsm_state current;
    pthread_mutex_t lock;
} fsm_handle;

// Public API
void fsm_init(fsm_handle* fsm, fsm_state initial_state);
void fsm_dispatch(fsm_handle* fsm, int event);
void fsm_destroy(fsm_handle* fsm);

#endif // FSM_H
