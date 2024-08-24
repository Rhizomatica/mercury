/* Shared memory routines
 *
 * Copyright (C) 2019-2024 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 */

#define _DEFAULT_SOURCE

#include "common/shm_posix.h"

#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <stdlib.h>

#if defined(_WIN32)

#include <io.h>
#define open _open
#define close _close
#define unlink _unlink
#define lseek _lseek
#define write _write

#define S_IXUSR  0000100
#define TMP_ENV_NAME "TEMP"
int get_temp_path(char* pathBuffer, int pathBufferSize, const char* pathPart)
{
	const char* temp = getenv(TMP_ENV_NAME);
	if(strlen(temp) >= pathBufferSize - strlen(pathPart+1)) {
		return 0;
	}

	/* We've done the size check above so we don't need to use the string safe methods */
	strcpy(pathBuffer, temp);
    strcat(pathBuffer, "\\");
	strcat(pathBuffer, pathPart+1);
	return 1;
}

#else

#include <sys/mman.h>
#include <sys/stat.h>        /* For mode constants */
#include <unistd.h>

#endif

// returns non-negative integer or negative if shm not created
int shm_open_and_get_fd(char *name)
{
    int fd = -1;

    if (strlen(name) >= MAX_POSIX_SHM_NAME)
    {
        fprintf(stderr, "ERROR: This should never happen! Name length bigger than allowed!\n");
        abort();
    }

#if defined(_WIN32)    /* open the file then memory map */
    // create pathBuffer
    char pathBuffer[PATH_MAX];
    if(!get_temp_path(pathBuffer, sizeof(pathBuffer), name))
    {
        printf("Error in get_temp_path()");
        return fd;
    }
    printf("Windows shm name: %s\n", pathBuffer);

    fd = open(pathBuffer, O_RDWR);
    if (fd < 0)
    {
        fprintf(stderr, "Shared memory not created. Aborting.\n");
        abort();
    }
#else
    // fprintf(stderr, "shm_open_and_get_fd() called with %s\n", name);
    fd = shm_open(name, O_RDWR, 0644);
#endif

    return fd;
}

// check if shm is already created, as this functions will unlink the shm if already created, and create a new one
// returns non-negative integer
int shm_create_and_get_fd(char *name, size_t size)
{
    int fd = -1;

    if (strlen(name) >= MAX_POSIX_SHM_NAME)
    {
        fprintf(stderr, "ERROR: This should never happen! Name length bigger than allowed!\n");
        abort();
    }

#if defined(_WIN32)
    char pathBuffer[PATH_MAX];
    if(!get_temp_path(pathBuffer, sizeof(pathBuffer), name))
    {
        printf("Error in get_temp_path()");
        return fd;
    }
    printf("Windows shm name: %s\n", pathBuffer);

    if ((fd = open(pathBuffer, O_RDWR, 0660 | S_IXUSR)) >= 0)
    {
        fprintf(stderr, "Windows shared memory already created. Re-creating it.\n");
        close(fd);
        unlink(pathBuffer);
    }

    fd = open(pathBuffer, O_CREAT | O_EXCL | O_RDWR, 0660 | S_IXUSR);
    if (fd < 0)
    {
        fprintf(stderr, "ERROR: This should never happen! SHM creation error in open!\n");
        abort();
    }

    if (lseek(fd, size - 1, SEEK_SET) == -1)
    {
        fprintf(stderr, "ERROR: This should never happen! SHM creation error in lseek\n");
        abort();
    }

    for (i = 0; i < size; i++)
    {
        if (write(fd, "", 1) == -1)
        {
            fprintf(stderr, "ERROR: This should never happen! SHM creation error in lseek\n");
            abort();
        }
    }


    // fprintf(stderr, "shm_create_and_get_fd() called with %s and %ld\n", name, size);
#else
    if (shm_open(name, O_RDWR, 0644) >= 0)
    {
        // fprintf(stderr, "POSIX shared memory already created. Re-creating it.\n");
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
#endif

    return fd;
}


// returns the pointer to the region, or (void *) -1 in case of error
void *shm_map(int fd, size_t size)
{
#if defined(_WIN32)
    printf("fd = %d\n", fd);

    HANDLE fmap = CreateFileMapping((HANDLE)_get_osfhandle(fd), NULL,
                                    PAGE_READWRITE, 0, 0, NULL);
    if (fmap == NULL)
    {
        printf("couldn't map %s\n", name);
        abort();
    }
    printf("after CreateFileMapping\n");

    void *data = (void *) MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, size);

    printf("after fmap == NULL test\n");

    return data;
#else
    return mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
#endif
}

// returns 0 on success, -1 on error
int shm_unmap(void *addr, size_t size)
{
#if defined(WIN32)

#if 0  // TODO: we need to have access to these variables...
    if (fmap != NULL)
    {
        if (mptr != NULL)
        {
            UnmapViewOfFile(mptr);
        }
        CloseHandle(fmap);
    }
#endif
    return 0;
#else
     return munmap(addr, size);
#endif

}
