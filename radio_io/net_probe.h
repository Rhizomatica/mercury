/* Is a network rig's host answering at all, within a bounded time?
 *
 * Copyright (C) 2026 Rhizomatica
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
 * ---------------------------------------------------------------------------
 *
 * Hamlib's network_open() connects with a blocking connect() and no timeout
 * of its own.  To a host that answers nothing -- powered off, off the Wi-Fi --
 * the kernel gives up only after its SYN retries: about 135 s on Linux's
 * defaults.  Mercury reopens a rig's session on the keying path with the
 * radio lock held, so before handing a host to rig_open() it asks, with a
 * non-blocking connect and a deadline, whether the host answers at all.
 *
 * "Answers" means the connect completed or was refused: a refused connect is
 * a live host saying no, and rig_open() will fail on it at once.  Only no
 * answer by the deadline, or an unreachable host or network, is "silent".
 *
 * A probe that connects leaves the server a connection to tear down, and a
 * server can get that wrong for whoever connects next.  rigctld up to Hamlib
 * 4.7 closes a client's descriptor three times (fclose of its read stream, of
 * its write stream, then close), so a client accepted in between is given
 * the same descriptor number and has it closed under it: rig_open() sent
 * right after a probe fails with "Broken pipe" (and does again at every
 * retry while the machine is busy enough to keep losing that race).
 *
 * Holding the probe open until the session is up would avoid that, but not
 * every server takes a second client while the first is connected: SDR++'s
 * rigctl server (which answers \chk_vfo and \dump_state, so Hamlib model 2
 * works against it) accepts the next one only when the current one ends, and
 * rig_open() would wait behind an idle probe until it timed out.  So
 * net_probe_host_hold() hands the connected socket back, and
 * net_probe_close_settled() closes it the slow way: half-close, wait for the
 * server's own close (rigctld sends its FIN from the first of its three
 * closes, SDR++ when its handler ends), then give the server a moment for the
 * rest of its teardown.  A client that connects after that finds the server
 * done with the probe.
 *
 * Header-only, free of Hamlib and of Mercury's other headers, so it builds
 * wherever radio_io.c does and can be unit-tested against local sockets.
 */

#ifndef NET_PROBE_H
#define NET_PROBE_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#endif

typedef enum
{
    NET_PROBE_ANSWERED = 0,   /* connected, or refused: the host is there */
    NET_PROBE_SILENT,         /* no answer by the deadline, or unreachable */
    NET_PROBE_UNKNOWN,        /* could not ask (resolve, socket): no opinion */
} net_probe_result_t;

static inline uint64_t net_probe_now_ms(void)
{
#if defined(_WIN32)
    return (uint64_t)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
#endif
}

#if defined(_WIN32)
typedef SOCKET net_probe_sock_t;
#define NET_PROBE_BAD_SOCK INVALID_SOCKET
#define net_probe_close(s) closesocket(s)
#else
typedef int net_probe_sock_t;
#define NET_PROBE_BAD_SOCK (-1)
#define net_probe_close(s) close(s)
#endif

/* One address: 1 answered, 0 silent, -1 could not try.  A connected socket
 * is handed to *held when held is not NULL, and closed otherwise. */
static inline int net_probe_addr(const struct addrinfo *ai, int wait_ms,
                                 net_probe_sock_t *held)
{
    net_probe_sock_t s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (s == NET_PROBE_BAD_SOCK)
        return -1;

#if defined(_WIN32)
    u_long nb = 1;
    if (ioctlsocket(s, FIONBIO, &nb) != 0)
    {
        net_probe_close(s);
        return -1;
    }
    if (connect(s, ai->ai_addr, (int)ai->ai_addrlen) == 0)
    {
        if (held)
            *held = s;
        else
            net_probe_close(s);
        return 1;
    }
    int err = WSAGetLastError();
    if (err == WSAECONNREFUSED)
    {
        net_probe_close(s);
        return 1;
    }
    if (err != WSAEWOULDBLOCK && err != WSAEINPROGRESS)
    {
        net_probe_close(s);
        return 0;                   /* unreachable host or network */
    }
    /* Winsock reports a completed connect in the write set and a failed one
     * in the except set. */
    fd_set wr, ex;
    FD_ZERO(&wr);
    FD_ZERO(&ex);
    FD_SET(s, &wr);
    FD_SET(s, &ex);
    struct timeval tv = { wait_ms / 1000, (wait_ms % 1000) * 1000 };
    int n = select(0, NULL, &wr, &ex, &tv);
    int answered = 0;
    if (n > 0 && FD_ISSET(s, &wr))
    {
        if (held)
        {
            *held = s;
            return 1;
        }
        answered = 1;
    }
    else if (n > 0)
    {
        int so = 0, len = (int)sizeof(so);
        getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&so, &len);
        answered = (so == WSAECONNREFUSED);
    }
    net_probe_close(s);
    return answered;
#else
    int fl = fcntl(s, F_GETFL, 0);
    if (fl < 0 || fcntl(s, F_SETFL, fl | O_NONBLOCK) < 0)
    {
        net_probe_close(s);
        return -1;
    }
    if (connect(s, ai->ai_addr, ai->ai_addrlen) == 0)
    {
        if (held)
            *held = s;
        else
            net_probe_close(s);
        return 1;
    }
    if (errno == ECONNREFUSED)
    {
        net_probe_close(s);
        return 1;
    }
    if (errno != EINPROGRESS)
    {
        net_probe_close(s);
        return 0;                   /* unreachable host or network */
    }
    struct pollfd pfd = { s, POLLOUT, 0 };
    int n;
    do
        n = poll(&pfd, 1, wait_ms);
    while (n < 0 && errno == EINTR);
    int answered = 0;
    if (n > 0)
    {
        int so = 0;
        socklen_t len = sizeof(so);
        if (getsockopt(s, SOL_SOCKET, SO_ERROR, &so, &len) == 0)
            answered = (so == 0 || so == ECONNREFUSED);
        if (so == 0 && answered && held)
        {
            *held = s;
            return 1;
        }
    }
    net_probe_close(s);
    return answered;
#endif
}

/* Does host:port answer within timeout_ms?  port may be "" (Hamlib's default
 * network port, 4532, is asked; any answer, a refusal included, shows the
 * host is up).  Every address the name resolves to is tried, sharing the one
 * deadline.  Resolving a name, as opposed to a numeric address, is the
 * system resolver's to bound.  When held is not NULL it is set to the
 * connected socket, or to NET_PROBE_BAD_SOCK when none is left open (silent,
 * refused, unknown); the caller closes it with net_probe_close(). */
static inline net_probe_result_t net_probe_host_hold(const char *host, const char *port,
                                                     int timeout_ms,
                                                     net_probe_sock_t *held)
{
    if (held)
        *held = NET_PROBE_BAD_SOCK;
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICSERV;

    if (!host || !host[0])
        return NET_PROBE_UNKNOWN;
    if (getaddrinfo(host, (port && port[0]) ? port : "4532", &hints, &res) != 0 || !res)
        return NET_PROBE_UNKNOWN;

    uint64_t deadline = net_probe_now_ms() + (uint64_t)(timeout_ms > 0 ? timeout_ms : 0);
    net_probe_result_t verdict = NET_PROBE_UNKNOWN;
    for (const struct addrinfo *ai = res; ai; ai = ai->ai_next)
    {
        uint64_t now = net_probe_now_ms();
        int left = now < deadline ? (int)(deadline - now) : 0;
        int r = net_probe_addr(ai, left, held);
        if (r == 1)
        {
            verdict = NET_PROBE_ANSWERED;
            break;
        }
        if (r == 0)
            verdict = NET_PROBE_SILENT;
        if (left == 0)
            break;
    }
    freeaddrinfo(res);
    return verdict;
}

static inline net_probe_result_t net_probe_host(const char *host, const char *port,
                                                int timeout_ms)
{
    return net_probe_host_hold(host, port, timeout_ms, NULL);
}

static inline void net_probe_sleep_ms(int ms)
{
#if defined(_WIN32)
    Sleep((DWORD)ms);
#else
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR)
        ;
#endif
}

/* Close a probe held by net_probe_host_hold() so that the server has let go
 * of it before anyone connects again (see above): shut our side, wait up to
 * wait_ms for the server to close its side, then settle_ms more.  Returns
 * true when the server was seen to close.  Anything it sent is discarded. */
static inline bool net_probe_close_settled(net_probe_sock_t s, int wait_ms, int settle_ms)
{
    bool closed = false;

    if (s == NET_PROBE_BAD_SOCK)
        return false;
#if defined(_WIN32)
    (void)shutdown(s, SD_SEND);
#else
    (void)shutdown(s, SHUT_WR);
#endif
    uint64_t deadline = net_probe_now_ms() + (uint64_t)(wait_ms > 0 ? wait_ms : 0);
    for (;;)
    {
        uint64_t now = net_probe_now_ms();
        if (now >= deadline)
            break;
        int left = (int)(deadline - now);
        char buf[256];
#if defined(_WIN32)
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(s, &rd);
        struct timeval tv = { left / 1000, (left % 1000) * 1000 };
        if (select(0, &rd, NULL, NULL, &tv) <= 0)
            break;
        int n = recv(s, buf, (int)sizeof buf, 0);
#else
        struct pollfd pfd = { s, POLLIN, 0 };
        int r = poll(&pfd, 1, left);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            break;
        ssize_t n = recv(s, buf, sizeof buf, 0);
        if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
            continue;
#endif
        if (n <= 0)
        {
            closed = true;          /* FIN (0) or reset (<0): it let go */
            break;
        }
    }
    net_probe_close(s);
    if (closed && settle_ms > 0)
        net_probe_sleep_ms(settle_ms);
    return closed;
}

#endif /* NET_PROBE_H */
