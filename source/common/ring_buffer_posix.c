/* Buffer code for use in IPC via shared memory
 * Copyright (C) 2020-2024 by Rafael Diniz <rafael@rhizomatica.org>
 * All Rights Reserved
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 */

#include "ring_buffer_posix.h"
#include "shm_posix.h"


#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>
#include <assert.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <sys/resource.h>
#include <sys/shm.h>
#include <string.h>
#include <sys/mman.h>
#include <malloc.h>
#include <pthread.h>
#include <unistd.h>

// Private functions

static void advance_pointer_n(cbuf_handle_t cbuf, size_t len)
{
    assert(cbuf);

    // fprintf(stderr, "head = %ld\n", cbuf->internal->head);

    if(cbuf->internal->full)
    {
        cbuf->internal->tail = (cbuf->internal->tail + len) % cbuf->internal->max;
    }

    cbuf->internal->head = (cbuf->internal->head + len) % cbuf->internal->max;

    // We mark full because we will advance tail on the next time around
    cbuf->internal->full = (cbuf->internal->head == cbuf->internal->tail);
}

static void advance_pointer(cbuf_handle_t cbuf)
{
    assert(cbuf && cbuf->internal);

    if(cbuf->internal->full)
    {
        cbuf->internal->tail = (cbuf->internal->tail + 1) % cbuf->internal->max;
    }

    cbuf->internal->head = (cbuf->internal->head + 1) % cbuf->internal->max;

    // We mark full because we will advance tail on the next time around
    cbuf->internal->full = (cbuf->internal->head == cbuf->internal->tail);
}

static void retreat_pointer_n(cbuf_handle_t cbuf, size_t len)
{
    assert(cbuf && cbuf->internal);

    cbuf->internal->full = false;
    cbuf->internal->tail = (cbuf->internal->tail + len) % cbuf->internal->max;
}

static void retreat_pointer(cbuf_handle_t cbuf)
{
    assert(cbuf->internal);

    cbuf->internal->full = false;
    cbuf->internal->tail = (cbuf->internal->tail + 1) % cbuf->internal->max;
}

size_t circular_buf_size_internal(cbuf_handle_t cbuf)
{
    assert(cbuf && cbuf->internal);

    size_t size = cbuf->internal->max;

    if(!cbuf->internal->full)
    {
        if(cbuf->internal->head >= cbuf->internal->tail)
        {
            size = (cbuf->internal->head - cbuf->internal->tail);
        }
        else
        {
            size = (cbuf->internal->max + cbuf->internal->head - cbuf->internal->tail);
        }

    }

    // fprintf(stderr, "size = %ld\n", size);
    return size;
}

size_t circular_buf_free_size_internal(cbuf_handle_t cbuf)
{
    assert(cbuf->internal);

    size_t size = 0;

    if(!cbuf->internal->full)
    {
        if(cbuf->internal->head >= cbuf->internal->tail)
        {
            size = cbuf->internal->max - (cbuf->internal->head - cbuf->internal->tail);
        }
        else
        {
            size = (cbuf->internal->tail - cbuf->internal->head);
        }

    }

    return size;
}


// !! Public User APIs !! //


cbuf_handle_t circular_buf_init_shm(size_t size, char *base_name)
{
    assert(size);
    char tmp[MAX_POSIX_SHM_NAME];
    int fd1, fd2;

    cbuf_handle_t cbuf = memalign(SHMLBA, sizeof(struct circular_buf_t));
    assert(cbuf);

    strcpy(tmp, base_name);
    strcat(tmp, "-1");
    fd1 = shm_create_and_get_fd(tmp, size);
    cbuf->buffer = shm_map(fd1, size);
    close(fd1);

    assert(cbuf->buffer);


    strcpy(tmp, base_name);
    strcat(tmp, "-2");
    fd2 = shm_create_and_get_fd(tmp, sizeof(struct circular_buf_t_aux));
    cbuf->internal = shm_map(fd2, sizeof(struct circular_buf_t_aux));
    close(fd2);

    assert(cbuf->internal);

    cbuf->internal->max = size;

    pthread_mutexattr_t mutex_attr;
    pthread_mutexattr_init(&mutex_attr);
    pthread_mutexattr_setpshared(&mutex_attr, PTHREAD_PROCESS_SHARED);
    pthread_condattr_t cond_attr;
    pthread_condattr_init(&cond_attr);
    pthread_condattr_setpshared(&cond_attr, PTHREAD_PROCESS_SHARED);
    pthread_mutex_init( &cbuf->internal->mutex, &mutex_attr);
    pthread_cond_init( &cbuf->internal->cond, &cond_attr);

    circular_buf_reset(cbuf);

    assert(circular_buf_empty(cbuf));

    return cbuf;
}

cbuf_handle_t circular_buf_connect_shm(size_t size, char *base_name)
{
    assert(size);
    char tmp[MAX_POSIX_SHM_NAME];
    int fd1, fd2;

    cbuf_handle_t cbuf = memalign(SHMLBA, sizeof(struct circular_buf_t));
    assert(cbuf);

    strcpy(tmp, base_name);
    strcat(tmp, "-1");
    fd1 = shm_open_and_get_fd(tmp);
    if (fd1 < 0)
        return NULL;
    cbuf->buffer = shm_map(fd1, size);
    close(fd1);

    assert(cbuf->buffer);


    strcpy(tmp, base_name);
    strcat(tmp, "-2");
    fd2 = shm_open_and_get_fd(tmp);
    if (fd2 < 0)
        return NULL;
    cbuf->internal = shm_map(fd2, sizeof(struct circular_buf_t_aux));
    close(fd2);

    assert(cbuf->internal);

    assert (cbuf->internal->max == size);

    return cbuf;
}

void circular_buf_free_shm(cbuf_handle_t cbuf, size_t size, char *base_name)
{
    assert(cbuf && cbuf->internal && cbuf->buffer);
    char tmp[MAX_POSIX_SHM_NAME];

    shm_unmap(cbuf->buffer, size);
    shm_unmap(cbuf->internal, sizeof(struct circular_buf_t_aux));

    strcpy(tmp, base_name);
    strcat(tmp, "-1");
    shm_unlink(tmp);

    strcpy(tmp, base_name);
    strcat(tmp, "-2");
    shm_unlink(tmp);

    free(cbuf);
}

void circular_buf_reset(cbuf_handle_t cbuf)
{
    assert(cbuf && cbuf->internal);

    pthread_mutex_lock( &cbuf->internal->mutex );

    cbuf->internal->head = 0;
    cbuf->internal->tail = 0;
    cbuf->internal->full = false;

    pthread_mutex_unlock( &cbuf->internal->mutex );
}

size_t size_buffer(cbuf_handle_t cbuf)
{
    assert(cbuf && cbuf->internal);

    pthread_mutex_lock( &cbuf->internal->mutex );

    size_t size = cbuf->internal->max;
    bool is_full = cbuf->internal->full;

    if(!is_full)
    {
        if(cbuf->internal->head >= cbuf->internal->tail)
        {
            size = (cbuf->internal->head - cbuf->internal->tail);
        }
        else
        {
            size = (cbuf->internal->max + cbuf->internal->head - cbuf->internal->tail);
        }
    }

    pthread_mutex_unlock( &cbuf->internal->mutex );

    // fprintf(stderr, "size = %ld\n", size);
    return size;
}

size_t circular_buf_free_size(cbuf_handle_t cbuf)
{
    assert(cbuf->internal);

    size_t size = 0;

    pthread_mutex_lock( &cbuf->internal->mutex );

    if(!cbuf->internal->full)
    {
        if(cbuf->internal->head >= cbuf->internal->tail)
        {
            size = cbuf->internal->max - (cbuf->internal->head - cbuf->internal->tail);
        }
        else
        {
            size = (cbuf->internal->tail - cbuf->internal->head);
        }

    }

    pthread_mutex_unlock( &cbuf->internal->mutex );

    return size;
}

size_t circular_buf_capacity(cbuf_handle_t cbuf)
{
    assert(cbuf->internal);

    pthread_mutex_lock( &cbuf->internal->mutex );

    size_t capacity = cbuf->internal->max;

    pthread_mutex_unlock( &cbuf->internal->mutex );

    return capacity;
}

int circular_buf_put(cbuf_handle_t cbuf, uint8_t data)
{
    assert(cbuf && cbuf->internal && cbuf->buffer);

    int r = -1;

try_again_put:
    pthread_mutex_lock( &cbuf->internal->mutex );

    bool is_full = cbuf->internal->full;

    if(!is_full)
    {
        cbuf->buffer[cbuf->internal->head] = data;
        advance_pointer(cbuf);
        r = 0;

        pthread_cond_signal( &cbuf->internal->cond );
        pthread_mutex_unlock( &cbuf->internal->mutex );
    }
    else
    {
        pthread_cond_wait( &cbuf->internal->cond, &cbuf->internal->mutex );
        pthread_mutex_unlock( &cbuf->internal->mutex );
        goto try_again_put;
    }

    return r;
}

int circular_buf_get(cbuf_handle_t cbuf, uint8_t * data)
{
    assert(cbuf && data && cbuf->internal && cbuf->buffer);

    int r = -1;

try_again_get:
    pthread_mutex_lock( &cbuf->internal->mutex );

    bool is_empty = !cbuf->internal->full && (cbuf->internal->head == cbuf->internal->tail);

    if(!is_empty)
    {
        *data = cbuf->buffer[cbuf->internal->tail];
        retreat_pointer(cbuf);
        r = 0;

        pthread_cond_signal( &cbuf->internal->cond );
        pthread_mutex_unlock( &cbuf->internal->mutex );
    }
    else
    {
        pthread_cond_wait( &cbuf->internal->cond, &cbuf->internal->mutex );
        pthread_mutex_unlock( &cbuf->internal->mutex );
        goto try_again_get;
    }

    return r;
}

bool circular_buf_empty(cbuf_handle_t cbuf)
{
    assert(cbuf && cbuf->internal);

    pthread_mutex_lock( &cbuf->internal->mutex );

    bool is_empty = !cbuf->internal->full && (cbuf->internal->head == cbuf->internal->tail);

    pthread_mutex_unlock( &cbuf->internal->mutex );

    return is_empty;
}

bool circular_buf_full(cbuf_handle_t cbuf)
{
    assert(cbuf && cbuf->internal);

    pthread_mutex_lock( &cbuf->internal->mutex );

    bool is_full = cbuf->internal->full;

    pthread_mutex_unlock( &cbuf->internal->mutex );

    return is_full;
}

int read_buffer(cbuf_handle_t cbuf, uint8_t *data, size_t len)
{
    assert(cbuf && data && cbuf->internal && cbuf->buffer);

    int r = -1;

 try_again_read:
    pthread_mutex_lock( &cbuf->internal->mutex );

    size_t size = cbuf->internal->max;

    if(circular_buf_size_internal(cbuf) >= len)
    {
        if ( ((cbuf->internal->tail + len) % size) > cbuf->internal->tail)
        {
            memcpy(data, cbuf->buffer + cbuf->internal->tail, len);
        }
        else
        {
            memcpy(data, cbuf->buffer + cbuf->internal->tail, size - cbuf->internal->tail);
            memcpy(data + (size - cbuf->internal->tail), cbuf->buffer, len - (size - cbuf->internal->tail));
        }
        retreat_pointer_n(cbuf, len);
        r = 0;

        pthread_cond_signal( &cbuf->internal->cond );
        pthread_mutex_unlock( &cbuf->internal->mutex );
    }
    else
    {
        pthread_cond_wait( &cbuf->internal->cond, &cbuf->internal->mutex );
        pthread_mutex_unlock( &cbuf->internal->mutex );
        goto try_again_read;
    }

    return r;
}

int write_buffer(cbuf_handle_t cbuf, uint8_t * data, size_t len)
{
    assert(cbuf && cbuf->internal && cbuf->buffer);

    int r = -1;

try_again_write:
    pthread_mutex_lock( &cbuf->internal->mutex );

    size_t size = cbuf->internal->max;

    if(circular_buf_free_size_internal(cbuf) >= len)
    {
        if ( ((cbuf->internal->head + len) % size) > cbuf->internal->head)
        {
            memcpy(cbuf->buffer + cbuf->internal->head, data, len);
        }
        else
        {
            memcpy(cbuf->buffer + cbuf->internal->head, data, size - cbuf->internal->head);
            memcpy(cbuf->buffer, data + (size - cbuf->internal->head), len - (size - cbuf->internal->head));
        }
        advance_pointer_n(cbuf, len);
        r = 0;

        pthread_cond_signal( &cbuf->internal->cond );
        pthread_mutex_unlock( &cbuf->internal->mutex );
    }
    else
    {
        pthread_cond_wait( &cbuf->internal->cond, &cbuf->internal->mutex );
        pthread_mutex_unlock( &cbuf->internal->mutex );
        goto try_again_write;
    }

    return r;
}


// this is the variant without inter-process shm, use these to init and free
// user needs to allocate buffer
cbuf_handle_t circular_buf_init(uint8_t* buffer, size_t size)
{
    assert(buffer && size);

    cbuf_handle_t cbuf = memalign(SHMLBA, sizeof(struct circular_buf_t));
    assert(cbuf);

    cbuf->internal = memalign(SHMLBA, sizeof(struct circular_buf_t_aux));
    assert(cbuf->internal);

    cbuf->buffer = buffer;
    cbuf->internal->max = size;
    circular_buf_reset(cbuf);

    pthread_mutex_init( &cbuf->internal->mutex, NULL );
    pthread_cond_init( &cbuf->internal->cond, NULL );

    assert(circular_buf_empty(cbuf));

    return cbuf;
}

// user needs to free previosly allocated buffer
void circular_buf_free(cbuf_handle_t cbuf)
{
    assert(cbuf && cbuf->internal);
    free(cbuf->internal);
    free(cbuf);
}
