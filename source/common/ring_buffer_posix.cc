/* Buffer code for use in IPC via shared memory
 *
 * Copyright (C) 2020-2024 Rhizomatica
 * Author: Rafael Diniz <rafael@rhizomatica.org>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 */

#include "common/ring_buffer_posix.h"
#include "common/shm_posix.h"


#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>
#include <assert.h>
#include <stdbool.h>
#include <string.h>

#include <time.h>
#include <malloc.h>

#if defined(_WIN32)

#include <io.h>
#define open _open
#define close _close
#define unlink _unlink
#define lseek _lseek
#define write _write

#else

#include <pthread.h>
#include <sys/resource.h>
#include <sys/shm.h>
#include <sys/mman.h>
#include <unistd.h>

#endif


// Reference from: https://github.com/marklakata/mqueue-w32
#if defined(_WIN32)
int MUTEX_LOCK(HANDLE *mqh_lock)
{
	DWORD dwWaitResult = WaitForSingleObject(
		mqh_lock,    // handle to mutex
		INFINITE);  // no time-out interval

    switch (dwWaitResult)
    {
        // The thread got ownership of the mutex
        case WAIT_OBJECT_0:
			return 0;

        // The thread got ownership of an abandoned mutex
        // The database is in an indeterminate state
        case WAIT_ABANDONED:
	  return ECANCELED ;
    default:
		  return EINVAL;
    }
}

/* Returns 0 on success */
int COND_WAIT(HANDLE *mqh_wait, HANDLE *mqh_lock)
{
	DWORD dwWaitResult;

	ReleaseMutex(mqh_lock);
	dwWaitResult = WaitForSingleObject(mqh_wait, INFINITE);
	if(dwWaitResult != WAIT_OBJECT_0) {
		return EINVAL;
	}

	return MUTEX_LOCK(mqh_lock);
}

/* Returns 0 on success */
int COND_TIMED_WAIT(HANDLE *mqh_wait, HANDLE *mqh_lock, const struct timespec* abstime)
{
	DWORD dwWaitResult;

	ReleaseMutex(mqh_lock);
	dwWaitResult = WaitForSingleObject(mqh_wait, (DWORD) abstime->tv_sec);
	switch(dwWaitResult) {
	    case WAIT_OBJECT_0:
	        break;
	    case WAIT_TIMEOUT:
	        return ETIMEDOUT;
	    default:
	        return EINVAL;
	}

	dwWaitResult = WaitForSingleObject(mqh_lock,(DWORD) abstime->tv_sec);
	if(dwWaitResult == WAIT_OBJECT_0) {
	    return 0;
	}

    if(dwWaitResult == WAIT_TIMEOUT) {
        return ETIMEDOUT;
    }

    return EINVAL;
}

/* Returns 0 on success */
int COND_SIGNAL(HANDLE *mqh_wait)
{
	BOOL result = SetEvent(mqh_wait);
	return result == 0;
}
void MUTEX_UNLOCK(HANDLE *mqh_lock)
{
	ReleaseMutex(mqh_lock);
}

#else
// #define INFINITE_ -1
#define MUTEX_LOCK(x)   pthread_mutex_lock(x)
#define MUTEX_UNLOCK(x) pthread_mutex_unlock(x)
#define COND_WAIT(x, y)  pthread_cond_wait(x, y)
#define COND_TIMED_WAIT(x, y, z) pthread_cond_timedwait(x, y, z)
#define COND_SIGNAL(x)  pthread_cond_signal(x)
#endif


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

#if !defined(_WIN32)
    cbuf_handle_t cbuf = (cbuf_handle_t) memalign(SHMLBA, sizeof(struct circular_buf_t));
#else
    cbuf_handle_t cbuf = (cbuf_handle_t) malloc(sizeof(struct circular_buf_t));
#endif
    assert(cbuf);

    strcpy(tmp, base_name);
    strcat(tmp, "-1");
    fd1 = shm_create_and_get_fd(tmp, size);
    cbuf->buffer = (uint8_t *) shm_map(fd1, size);

    // TODO: should we close it on Windows?
#if !defined(_WIN32)
    close(fd1);
#endif

    assert(cbuf->buffer);


    strcpy(tmp, base_name);
    strcat(tmp, "-2");
    fd2 = shm_create_and_get_fd(tmp, sizeof(struct circular_buf_t_aux));
    cbuf->internal = (struct circular_buf_t_aux *) shm_map(fd2, sizeof(struct circular_buf_t_aux));
#if !defined(_WIN32)
    close(fd2);
#endif

    assert(cbuf->internal);

    cbuf->internal->max = size;

#if defined(_WIN32)
    //    wchar_t nameBuffer[128];
    char nameBuffer[MAX_POSIX_SHM_NAME];
    size_t len;

    len = strlen(tmp);
    if (len >= MAX_POSIX_SHM_NAME)
    {
        errno = EINVAL;
        printf("path name too long\n");
        return NULL;
    }

    sprintf(nameBuffer, "Global\\HERMES_Mtx%s", tmp+1);

    cbuf->internal->mutex = CreateMutex(
					NULL,              // default security attributes
					FALSE,             // initially not owned
					nameBuffer);       // named mutex

    sprintf(nameBuffer, "Global\\HERMES_Cond%s", tmp+1);

    cbuf->internal->cond = CreateEvent(
				       NULL,
				       FALSE,
				       FALSE,
				       nameBuffer
				       );

#else

    pthread_mutexattr_t mutex_attr;
    pthread_mutexattr_init(&mutex_attr);
    pthread_mutexattr_setpshared(&mutex_attr, PTHREAD_PROCESS_SHARED);

    pthread_condattr_t cond_attr;
    pthread_condattr_init(&cond_attr);
    pthread_condattr_setpshared(&cond_attr, PTHREAD_PROCESS_SHARED);

    pthread_mutex_init( &cbuf->internal->mutex, &mutex_attr);
    pthread_cond_init( &cbuf->internal->cond, &cond_attr);

    pthread_mutexattr_destroy(&mutex_attr);
    pthread_condattr_destroy(&cond_attr);

#endif

    circular_buf_reset(cbuf);

    assert(circular_buf_empty(cbuf));

    return cbuf;
}

cbuf_handle_t circular_buf_connect_shm(size_t size, char *base_name)
{
    assert(size);
    char tmp[MAX_POSIX_SHM_NAME];
    int fd1, fd2;

#if !defined(_WIN32)
    cbuf_handle_t cbuf = (cbuf_handle_t) memalign(SHMLBA, sizeof(struct circular_buf_t));
#else
    cbuf_handle_t cbuf = (cbuf_handle_t) malloc(sizeof(struct circular_buf_t));
#endif
    assert(cbuf);

    strcpy(tmp, base_name);
    strcat(tmp, "-1");

    fd1 = shm_open_and_get_fd(tmp);

    if (fd1 < 0)
        return NULL;
    cbuf->buffer = (uint8_t *) shm_map(fd1, size);
    close(fd1);

    assert(cbuf->buffer);


    strcpy(tmp, base_name);
    strcat(tmp, "-2");
    fd2 = shm_open_and_get_fd(tmp);
    if (fd2 < 0)
        return NULL;
    cbuf->internal = (struct circular_buf_t_aux *) shm_map(fd2, sizeof(struct circular_buf_t_aux));
    close(fd2);

    assert(cbuf->internal);

    assert (cbuf->internal->max == size);

    return cbuf;
}

void circular_buf_free_shm(cbuf_handle_t cbuf)
{
    free(cbuf);
}

void circular_buf_destroy_shm(cbuf_handle_t cbuf, size_t size, char *base_name)
{
    assert(cbuf && cbuf->internal && cbuf->buffer);
    char tmp[MAX_POSIX_SHM_NAME];

    // TODO: wire up the shutdown for windows stuff...
#if !defined(_WIN32)
    shm_unmap(cbuf->buffer, size);
    shm_unmap(cbuf->internal, sizeof(struct circular_buf_t_aux));

    strcpy(tmp, base_name);
    strcat(tmp, "-1");
    shm_unlink(tmp);

    strcpy(tmp, base_name);
    strcat(tmp, "-2");
    shm_unlink(tmp);
#endif
}

void circular_buf_reset(cbuf_handle_t cbuf)
{
    assert(cbuf && cbuf->internal);

    MUTEX_LOCK( &cbuf->internal->mutex );

    cbuf->internal->head = 0;
    cbuf->internal->tail = 0;
    cbuf->internal->full = false;

    MUTEX_UNLOCK( &cbuf->internal->mutex );
}

size_t size_buffer(cbuf_handle_t cbuf)
{
    assert(cbuf && cbuf->internal);

    MUTEX_LOCK( &cbuf->internal->mutex );

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

    MUTEX_UNLOCK( &cbuf->internal->mutex );

    // fprintf(stderr, "size = %ld\n", size);
    return size;
}

size_t circular_buf_free_size(cbuf_handle_t cbuf)
{
    assert(cbuf->internal);

    size_t size = 0;

    MUTEX_LOCK( &cbuf->internal->mutex );

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

    MUTEX_UNLOCK( &cbuf->internal->mutex );

    return size;
}

size_t circular_buf_capacity(cbuf_handle_t cbuf)
{
    assert(cbuf->internal);

    MUTEX_LOCK( &cbuf->internal->mutex );

    size_t capacity = cbuf->internal->max;

    MUTEX_UNLOCK( &cbuf->internal->mutex );

    return capacity;
}

int circular_buf_put(cbuf_handle_t cbuf, uint8_t data)
{
    assert(cbuf && cbuf->internal && cbuf->buffer);

    int r = -1;

try_again_put:
    MUTEX_LOCK( &cbuf->internal->mutex );

    bool is_full = cbuf->internal->full;

    if(!is_full)
    {
        cbuf->buffer[cbuf->internal->head] = data;
        advance_pointer(cbuf);
        r = 0;

        COND_SIGNAL( &cbuf->internal->cond );
        MUTEX_UNLOCK( &cbuf->internal->mutex );
    }
    else
    {
        COND_WAIT( &cbuf->internal->cond, &cbuf->internal->mutex );
        MUTEX_UNLOCK( &cbuf->internal->mutex );
        goto try_again_put;
    }

    return r;
}

int circular_buf_get(cbuf_handle_t cbuf, uint8_t * data)
{
    assert(cbuf && data && cbuf->internal && cbuf->buffer);

    int r = -1;

try_again_get:
    MUTEX_LOCK( &cbuf->internal->mutex );

    bool is_empty = !cbuf->internal->full && (cbuf->internal->head == cbuf->internal->tail);

    if(!is_empty)
    {
        *data = cbuf->buffer[cbuf->internal->tail];
        retreat_pointer(cbuf);
        r = 0;

        COND_SIGNAL( &cbuf->internal->cond );
        MUTEX_UNLOCK( &cbuf->internal->mutex );
    }
    else
    {
        COND_WAIT( &cbuf->internal->cond, &cbuf->internal->mutex );
        MUTEX_UNLOCK( &cbuf->internal->mutex );
        goto try_again_get;
    }

    return r;
}

bool circular_buf_empty(cbuf_handle_t cbuf)
{
    assert(cbuf && cbuf->internal);

    MUTEX_LOCK( &cbuf->internal->mutex );

    bool is_empty = !cbuf->internal->full && (cbuf->internal->head == cbuf->internal->tail);

    MUTEX_UNLOCK( &cbuf->internal->mutex );

    return is_empty;
}

bool circular_buf_full(cbuf_handle_t cbuf)
{
    assert(cbuf && cbuf->internal);

    MUTEX_LOCK( &cbuf->internal->mutex );

    bool is_full = cbuf->internal->full;

    MUTEX_UNLOCK( &cbuf->internal->mutex );

    return is_full;
}

int read_buffer_all(cbuf_handle_t cbuf, uint8_t *data)
{
    assert(cbuf && data && cbuf->internal && cbuf->buffer);

    size_t size = 0;
    size_t len = 0;

 try_again_read:
    MUTEX_LOCK( &cbuf->internal->mutex );

    size = cbuf->internal->max;
    len = circular_buf_size_internal(cbuf);

    if(len > 0)
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

        COND_SIGNAL( &cbuf->internal->cond );
        MUTEX_UNLOCK( &cbuf->internal->mutex );
    }
    else
    {
        COND_WAIT( &cbuf->internal->cond, &cbuf->internal->mutex );
        MUTEX_UNLOCK( &cbuf->internal->mutex );
        goto try_again_read;
    }

    return len;
}

#if 0
int buffer_retreat_and_unlock(cbuf_handle_t cbuf, size_t len)
{
    assert(cbuf && len);

    size_t size = cbuf->internal->max;

    retreat_pointer_n(cbuf, len);

    COND_SIGNAL( &cbuf->internal->cond );
    MUTEX_UNLOCK( &cbuf->internal->mutex );

    return 0;
}

int read_buffer_no_retreat_and_lock(cbuf_handle_t cbuf, uint8_t *data, size_t len)
{
    assert(cbuf && data && cbuf->internal && cbuf->buffer);

    int r = -1;

 try_again_read:
    MUTEX_LOCK( &cbuf->internal->mutex );

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
        r = 0;

        // COND_SIGNAL( &cbuf->internal->cond );
        // MUTEX_UNLOCK( &cbuf->internal->mutex );
    }
    else
    {
        COND_WAIT( &cbuf->internal->cond, &cbuf->internal->mutex );
        MUTEX_UNLOCK( &cbuf->internal->mutex );
        goto try_again_read;
    }

    return r;
}
#endif

int read_buffer(cbuf_handle_t cbuf, uint8_t *data, size_t len)
{
    assert(cbuf && data && cbuf->internal && cbuf->buffer);

    int r = -1;

 try_again_read:
    MUTEX_LOCK( &cbuf->internal->mutex );

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

        COND_SIGNAL( &cbuf->internal->cond );
        MUTEX_UNLOCK( &cbuf->internal->mutex );
    }
    else
    {
        COND_WAIT( &cbuf->internal->cond, &cbuf->internal->mutex );
        MUTEX_UNLOCK( &cbuf->internal->mutex );
        goto try_again_read;
    }

    return r;
}

int write_buffer(cbuf_handle_t cbuf, uint8_t * data, size_t len)
{
    assert(cbuf && cbuf->internal && cbuf->buffer);

    int r = -1;

try_again_write:
    MUTEX_LOCK( &cbuf->internal->mutex );

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

        COND_SIGNAL( &cbuf->internal->cond );
        MUTEX_UNLOCK( &cbuf->internal->mutex );
    }
    else
    {
        COND_WAIT( &cbuf->internal->cond, &cbuf->internal->mutex );
        MUTEX_UNLOCK( &cbuf->internal->mutex );
        goto try_again_write;
    }

    return r;
}


// this is the variant without inter-process shm, use these to init and free
// user needs to allocate buffer
cbuf_handle_t circular_buf_init(uint8_t* buffer, size_t size)
{
    assert(buffer && size);

#if defined(_WIN32)
    cbuf_handle_t cbuf = (cbuf_handle_t) malloc(sizeof(struct circular_buf_t));
    assert(cbuf);
    cbuf->internal = (struct circular_buf_t_aux *) malloc(sizeof(struct circular_buf_t_aux));
    assert(cbuf->internal);
#else
    cbuf_handle_t cbuf = (cbuf_handle_t) memalign(SHMLBA, sizeof(struct circular_buf_t));
    assert(cbuf);
    cbuf->internal = (struct circular_buf_t_aux *) memalign(SHMLBA, sizeof(struct circular_buf_t_aux));
    assert(cbuf->internal);
#endif

    cbuf->buffer = buffer;
    cbuf->internal->max = size;
    circular_buf_reset(cbuf);
    assert(circular_buf_empty(cbuf));

#if defined(_WIN32)
    cbuf->internal->mutex = CreateMutex(
					NULL,              // default security attributes
					FALSE,             // initially not owned
					NULL);       // named mutex

    cbuf->internal->cond = CreateEvent(
				       NULL,
				       FALSE,
				       FALSE,
				       NULL
				       );
#else
    pthread_mutex_init( &cbuf->internal->mutex, NULL );
    pthread_cond_init( &cbuf->internal->cond, NULL );
#endif
    
    

    return cbuf;
}

// user needs to free previosly allocated buffer
void circular_buf_free(cbuf_handle_t cbuf)
{
    assert(cbuf && cbuf->internal);
    free(cbuf->internal);
    free(cbuf);
}
