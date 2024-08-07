/* Shared memory routines
 *
 * Copyright (C) 2019-2024 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 */

#define _DEFAULT_SOURCE

#include "shm_posix.h"

#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>        /* For mode constants */
#include <fcntl.h>           /* For O_* constants */
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>

// returns non-negative integer or negative if shm not created
int shm_open_and_get_fd(char *name)
{
    int fd = -1;

    fprintf(stderr, "shm_open_and_get_fd() called with %s\n", name);

    if (strlen(name) >= MAX_POSIX_SHM_NAME)
    {
        fprintf(stderr, "ERROR: This should never happen! Name length bigger than allowed!\n");
        abort();
    }

    fd = shm_open(name, O_RDWR, 0644);

    return fd;
}

// check if shm is already created, as this functions will unlink the shm if already created, and create a new one
// returns non-negative integer
int shm_create_and_get_fd(char *name, size_t size)
{
    int fd = -1;

    fprintf(stderr, "shm_create_and_get_fd() called with %s and %ld\n", name, size);

    if (shm_open(name, O_RDWR, 0644) >= 0)
    {
        fprintf(stderr, "POSIX shared memory already created\nRe-creating it.\n");
        shm_unlink(name);
    }

    if((fd = shm_open(name, O_RDWR | O_CREAT, 0644)) == -1)
    {
        fprintf(stderr, "ERROR: This should never happen! SHM creation error!\n");
        abort();
    }

    if (ftruncate (fd, size) == -1)
    {
        fprintf(stderr, "ERROR: This should never happen! Error in ftruncate!\n");
        abort();
    }

    return fd;
}


// returns the pointer to the region, or (void *) -1 in case of error
void *shm_map(int fd, size_t size)
{
    return mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
}

// returns 0 on success, -1 on error
int shm_unmap(void *addr, size_t size)
{
    return munmap(addr, size);
}

