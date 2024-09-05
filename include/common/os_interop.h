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

union sigval {
    int           sival_int;     /* integer value */
    void          *sival_ptr;    /* pointer value */
};
struct sigevent {
    int           sigev_notify;  /* notification type */
    int           sigev_signo;   /* signal number */
    union sigval  sigev_value;   /* signal value */
};


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

int MUTEX_LOCK(HANDLE *mqh_lock)
{
	DWORD dwWaitResult = WaitForSingleObject(
		mqh_lock,    // handle to mutex
		-1);  // no time-out interval

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
	dwWaitResult = WaitForSingleObject(mqh_wait, -1);
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

#define O_NONBLOCK  0200000

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
#define O_RDONLY        _O_RDONLY
#define O_BINARY        _O_BINARY
#define O_CREAT         _O_CREAT
#define O_WRONLY        _O_WRONLY
#define O_TRUNC         _O_TRUNC
#define S_IREAD         _S_IREAD
#define S_IWRITE        _S_IWRITE
#define S_IFDIR         _S_IFDIR

#define S_IXUSR  0000100
#define TMP_ENV_NAME "TEMP"


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
