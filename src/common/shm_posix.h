/* Shared memory routines
 *
 * Copyright (C) 2019-2024 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 */

#pragma once

#if defined(_WIN32)
#include <winsock2.h>
#include <windows.h>
#else
#include <sys/ipc.h>
#include <sys/shm.h>
#endif

#include <stdbool.h>

#if defined(__ANDROID__)
/* bionic has no POSIX shared memory.  Only the shm audio subsystem (-x shm)
 * uses it, to meet another process, and an Android app has none: it fails
 * there at start-up. */
#include <errno.h>
#include <sys/types.h>
static inline int shm_open(const char *name, int oflag, mode_t mode)
{
    (void) name; (void) oflag; (void) mode;
    errno = ENOSYS;
    return -1;
}
static inline int shm_unlink(const char *name)
{
    (void) name;
    errno = ENOSYS;
    return -1;
}
#endif

#ifdef __cplusplus
extern "C" {
#endif

// name should be max 255
#define MAX_POSIX_SHM_NAME 255

// returns non-negative integer file descriptor or negative if shm not created
int shm_open_and_get_fd(char *name);

// check if shm is already created, as this functions will unlink the shm if already created, and create a new one
// returns non-negative integer
int shm_create_and_get_fd(char *name, size_t size);

// returns the pointer to the region, or (void *) -1 in case of error
void *shm_map(int fd, size_t size);

// returns 0 on success, -1 on error
int shm_unmap(void *addr, size_t size);

#ifdef __cplusplus
};
#endif
