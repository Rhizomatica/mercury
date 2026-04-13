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
#include <ws2tcpip.h>
#include <windows.h>
#include <pthread.h>
#include <io.h>
#include <time.h>

/* Socket compatibility */
#define SOCK_CLOSE(fd) closesocket(fd)
#define SOCK_IOCTL(fd, cmd, argp) ioctlsocket((SOCKET)(fd), (long)(cmd), (u_long*)(argp))
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
typedef ULONG nfds_t;

/* WSAPoll() has known bugs on various Windows versions (missed POLLIN
 * events, silent failures).  Use select() instead — it is the original
 * Winsock multiplexing API and is reliable everywhere. */
static inline int hermes_poll(struct pollfd *fds, nfds_t nfds, int timeout_ms)
{
    fd_set rd, er;
    struct timeval tv, *tvp;
    nfds_t i;
    int ready;

    FD_ZERO(&rd);
    FD_ZERO(&er);

    for (i = 0; i < nfds; i++)
    {
        fds[i].revents = 0;
        if (fds[i].fd == INVALID_SOCKET)
            continue;
        if (fds[i].events & (POLLIN | POLLRDNORM | POLLRDBAND))
            FD_SET(fds[i].fd, &rd);
        FD_SET(fds[i].fd, &er);
    }

    if (timeout_ms < 0)
    {
        tvp = NULL;
    }
    else
    {
        tv.tv_sec  = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        tvp = &tv;
    }

    /* First arg is ignored on Windows. */
    ready = select(0, &rd, NULL, &er, tvp);
    if (ready <= 0)
        return ready;

    for (i = 0; i < nfds; i++)
    {
        if (fds[i].fd == INVALID_SOCKET)
            continue;
        if (FD_ISSET(fds[i].fd, &rd))
            fds[i].revents |= POLLIN;
        if (FD_ISSET(fds[i].fd, &er))
            fds[i].revents |= POLLERR;
    }

    return ready;
}
#define poll(fds, nfds, timeout) hermes_poll((fds), (nfds), (timeout))

static inline int sock_set_nonblocking(int fd)
{
    u_long one = 1;
    return ioctlsocket((SOCKET)fd, FIONBIO, &one) == 0 ? 0 : -1;
}

static inline int sock_errno(void) { return WSAGetLastError(); }
#define SOCK_EAGAIN      WSAEWOULDBLOCK
#define SOCK_EWOULDBLOCK WSAEWOULDBLOCK
#define SOCK_EINTR       WSAEINTR

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
#include <sys/ioctl.h>
#include <fcntl.h>
#include <errno.h>

/* Socket compatibility */
#define SOCK_CLOSE(fd) close(fd)
#define SOCK_IOCTL(fd, cmd, argp) ioctl((fd), (unsigned long)(cmd), (argp))

static inline int sock_set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static inline int sock_errno(void) { return errno; }
#define SOCK_EAGAIN      EAGAIN
#define SOCK_EWOULDBLOCK EWOULDBLOCK
#define SOCK_EINTR       EINTR

#define MUTEX_LOCK(x)   pthread_mutex_lock(x)
#define MUTEX_UNLOCK(x) pthread_mutex_unlock(x)
#define COND_WAIT(x, y)  pthread_cond_wait(x, y)
#define COND_TIMED_WAIT(x, y, z) pthread_cond_timedwait(x, y, z)
#define COND_SIGNAL(x)  pthread_cond_signal(x)

#endif

#ifdef __cplusplus
};
#endif
