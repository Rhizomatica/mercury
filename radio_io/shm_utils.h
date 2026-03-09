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


#ifndef HAVE_SHM_UTILS_H__
#define HAVE_SHM_UTILS_H__

#include <stdio.h>
#include <inttypes.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <stdbool.h>

bool shm_is_created(key_t key, size_t size);

// only creates if already not created!
bool shm_create(key_t key, size_t size);

bool shm_destroy(key_t key, size_t size);

void *shm_attach(key_t key, size_t size);

bool shm_dettach(key_t key, size_t size, void *ptr);

#endif // HAVE_SHM_UTILS_H__
