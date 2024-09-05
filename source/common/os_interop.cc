/* Windows/Linux interoperability layer
 *
 * Copyright (C) 2020-2024 Rhizomatica
 * Author: Rafael Diniz <rafael@rhizomatica.org>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 */

#include "common/os_interop.h"

#if defined(_WIN32)

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

#endif
