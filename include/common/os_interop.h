/* Windows/Linux interoperability layer
 *
 * Copyright (C) 2020-2024 Rhizomatica
 * Author: Rafael Diniz <rafael@rhizomatica.org>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 */

#pragma once


// threading support

#if defined(_WIN32)

#include <winsock2.h>
#include <windows.h>
#include <io.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif


union sigval {
    int           sival_int;     /* integer value */
    void          *sival_ptr;    /* pointer value */
};
struct sigevent {
    int           sigev_notify;  /* notification type */
    int           sigev_signo;   /* signal number */
    union sigval  sigev_value;   /* signal value */
};

int get_temp_path(char* pathBuffer, int pathBufferSize, const char* pathPart);
int MUTEX_LOCK(HANDLE *mqh_lock);
void MUTEX_UNLOCK(HANDLE *mqh_lock);
/* Returns 0 on success */
int COND_WAIT(HANDLE *mqh_wait, HANDLE *mqh_lock);
/* Returns 0 on success */
int COND_TIMED_WAIT(HANDLE *mqh_wait, HANDLE *mqh_lock, const struct timespec* abstime);
/* Returns 0 on success */
int COND_SIGNAL(HANDLE *mqh_wait);



#define TMP_ENV_NAME "TEMP"

#define O_NONBLOCK  0200000

#if 0
#define open            _open
#define read            _read
#define write           _write
#define close           _close
#define stat            _stat
#define fstat           _fstat
#define mkdir           _mkdir
#define snprintf        _snprintf
#define unlink _unlink
#define lseek _lseek
#if _MSC_VER <= 1200 /* Versions below VC++ 6 */
#define vsnprintf       _vsnprintf
#endif
#endif

#define O_RDONLY        _O_RDONLY
#define O_BINARY        _O_BINARY
#define O_CREAT         _O_CREAT
#define O_WRONLY        _O_WRONLY
#define O_TRUNC         _O_TRUNC
#define S_IREAD         _S_IREAD
#define S_IWRITE        _S_IWRITE
#define S_IFDIR         _S_IFDIR

#define S_IXUSR  0000100

#ifdef __cplusplus
}
#endif


#else

#include <pthread.h>
#include <sys/resource.h>
#include <sys/shm.h>
#include <sys/mman.h>
#include <sys/stat.h>        /* For mode constants */
#include <unistd.h>

#define MUTEX_LOCK(x)   pthread_mutex_lock(x)
#define MUTEX_UNLOCK(x) pthread_mutex_unlock(x)
#define COND_WAIT(x, y)  pthread_cond_wait(x, y)
#define COND_TIMED_WAIT(x, y, z) pthread_cond_timedwait(x, y, z)
#define COND_SIGNAL(x)  pthread_cond_signal(x)


#endif

#ifdef __cplusplus
extern "C" {
#endif

// portable glibc-based srand/rand
long int __random (void);
void __srandom (unsigned int x);

#ifdef __cplusplus
};
#endif
