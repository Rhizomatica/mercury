/* Shared memory functions
 *
 * Copyright (C) 2019-2024 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3, or (at your option)
 * any later version.
 *
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this software; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street,
 * Boston, MA 02110-1301, USA.
 *
 */

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ipc.h>
#include <sys/shm.h>

#include "shm_utils.h"

bool shm_is_created(key_t key, size_t size)
{
    int shmid = shmget(key, size, 0);

    if (shmid == -1)
    {
        return false;
    }

    return true;
}

// check of key is already not created before calling this!
bool shm_create(key_t key, size_t size)
{
    int shmid = shmget(key, size, 0666 | IPC_CREAT | IPC_EXCL);

    if (shmid == -1)
    {
        return false;
    }

    return true;
}

bool shm_destroy(key_t key, size_t size)
{
    int shmid = shmget(key, size, 0);

    if (shmid == -1)
    {
        return false;
    }

    shmctl(shmid,IPC_RMID,NULL);

    return true;
}

void *shm_attach(key_t key, size_t size)
{
    int shmid = shmget(key, size, 0);
    if (shmid == -1)
        return NULL;


    void *tmp = shmat(shmid, NULL, 0);
    if (tmp == (void *)-1)
        return NULL;

    return tmp;
}

bool shm_dettach(key_t key, size_t size, void *ptr)
{
    int shmid = shmget(key, size, 0);
    if (shmid == -1)
        return false;

    if (!ptr)
        return false;

    shmid = shmdt(ptr);
    if (shmid == -1)
        return false;

    return true;
}
