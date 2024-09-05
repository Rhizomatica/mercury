/* Buffer code for use in IPC via shared memory
 *
 * Copyright (C) 2020-2024 Rhizomatica
 * Author: Rafael Diniz <rafael@rhizomatica.org>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

// this includes pthreads
#include "os_interop.h"

#ifdef __cplusplus
extern "C" {
#endif

struct circular_buf_t_aux {
    size_t head;
    size_t tail;
    size_t max; //of the buffer
    bool full;

#if defined(_WIN32)
    HANDLE            mutex;  /* mutex lock */
    HANDLE            cond;  /* and condition event */
#else
    pthread_mutex_t   mutex;  /* mutex lock */
    pthread_cond_t    cond;  /* and condition variable */
#endif
};

struct circular_buf_t {
    struct circular_buf_t_aux *internal;
    uint8_t *buffer;
};

/// Handle type, the way users interact with the API
typedef struct circular_buf_t* cbuf_handle_t;

// +++ The next 8 functions should be enough to do everything +++

// Shared memory init (create) function: 2 shared memory objects are created: base_name-1 and basename-2.
cbuf_handle_t circular_buf_init_shm(size_t size, char *base_name);

// function to connect to an already created shared memory area
cbuf_handle_t circular_buf_connect_shm(size_t size, char *base_name);

// unmaps and unlink (destroy) the shared memory (destroys the shared memory space)
void circular_buf_destroy_shm(cbuf_handle_t cbuf, size_t size, char *base_name);

//  free the process local cbuf_handle_t
void circular_buf_free_shm(cbuf_handle_t cbuf);

// returns 0 on success, -1 on error (blocking version)
int read_buffer(cbuf_handle_t cbuf, uint8_t *data, size_t len);

// returns the number of bytes read on success, -1 on error (blocking version)
int read_buffer_all(cbuf_handle_t cbuf, uint8_t *data);

// returns 0 on success, -1 on error (blocking version)
int write_buffer(cbuf_handle_t cbuf, uint8_t * data, size_t len);

/// Returns the number of elements stored in the buffer
/// Requires: cbuf is valid and created by circular_buf_init
/// Returns the current number of elements in the buffer
size_t size_buffer(cbuf_handle_t cbuf);

// clear the buffer
void clear_buffer(cbuf_handle_t cbuf);

/// Reset the circular buffer to empty, head == tail. Data not cleared
/// Requires: cbuf is valid and created by circular_buf_init
void circular_buf_reset(cbuf_handle_t cbuf);

/// Put a value in the buffer
/// Requires: cbuf is valid and created by circular_buf_init
/// Returns 0 on success, -1 if buffer is full
int circular_buf_put(cbuf_handle_t cbuf, uint8_t data);

/// Retrieve a value from the buffer
/// Requires: cbuf is valid and created by circular_buf_init
/// Returns 0 on success, -1 if the buffer is empty
int circular_buf_get(cbuf_handle_t cbuf, uint8_t * data);

/// CHecks if the buffer is empty
/// Requires: cbuf is valid and created by circular_buf_init
/// Returns true if the buffer is empty
bool circular_buf_empty(cbuf_handle_t cbuf);

/// Checks if the buffer is full
/// Requires: cbuf is valid and created by circular_buf_init
/// Returns true if the buffer is full
bool circular_buf_full(cbuf_handle_t cbuf);

/// Check the capacity of the buffer
/// Requires: cbuf is valid and created by circular_buf_init
/// Returns the maximum capacity of the buffer
size_t circular_buf_capacity(cbuf_handle_t cbuf);

/// Check the number of free elements in the buffer
/// Requires: cbuf is valid and created by circular_buf_init
/// Returns the current number of free elements in the buffer
size_t circular_buf_free_size(cbuf_handle_t cbuf);



// This are the init/free variants without using shared memory for IPC

/// Pass in a storage buffer and size, returns a circular buffer handle
/// Requires: buffer is not NULL, size > 0
/// Ensures: cbuf has been created and is returned in an empty state
cbuf_handle_t circular_buf_init(uint8_t *buffer, size_t size);

/// Free a circular buffer structure
/// Requires: cbuf is valid and created by circular_buf_init
/// Does not free data buffer; owner is responsible for that
void circular_buf_free(cbuf_handle_t cbuf);

#ifdef __cplusplus
};
#endif
