/* net_probe.h: does a network rig's host answer, within a bounded time?
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
 * Mercury reopens a network rig's Hamlib session on the keying path, and
 * Hamlib's connect has no timeout: to a host that is gone the kernel tries for
 * ~135 s.  The probe must say "silent" by its deadline, and must never call a
 * live host silent -- a refused port is a live host.
 *
 * "Gone" is made locally with a listener whose accept queue is full: Linux
 * then drops further SYNs, which is what a powered-off host looks like.  That
 * is Linux behaviour, so that one case is skipped elsewhere.
 */

#include "unity.h"
#include "net_probe.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>

void setUp(void) {}
void tearDown(void) {}

/* A listening socket on 127.0.0.1 with the given backlog; its port in *port. */
static int listen_local(int backlog, char port[8])
{
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    socklen_t len = sizeof(a);

    TEST_ASSERT_TRUE(s >= 0);
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    TEST_ASSERT_EQUAL_INT(0, bind(s, (struct sockaddr *)&a, sizeof(a)));
    TEST_ASSERT_EQUAL_INT(0, listen(s, backlog));
    TEST_ASSERT_EQUAL_INT(0, getsockname(s, (struct sockaddr *)&a, &len));
    snprintf(port, 8, "%u", (unsigned)ntohs(a.sin_port));
    return s;
}

void test_listening_host_answers(void)
{
    char port[8];
    int s = listen_local(4, port);

    TEST_ASSERT_EQUAL_INT(NET_PROBE_ANSWERED, net_probe_host("127.0.0.1", port, 1000));
    close(s);
}

void test_refused_port_is_a_live_host(void)
{
    char port[8];
    int s = listen_local(4, port);
    close(s);                       /* nothing listens there now: RST */

    uint64_t t0 = net_probe_now_ms();
    TEST_ASSERT_EQUAL_INT(NET_PROBE_ANSWERED, net_probe_host("127.0.0.1", port, 1000));
    TEST_ASSERT_TRUE(net_probe_now_ms() - t0 < 500);
}

void test_silent_host_is_reported_by_the_deadline(void)
{
#if defined(__linux__)
    char port[8];
    int s = listen_local(0, port);
    int fill[4];
    struct sockaddr_in a;

    /* Fill the accept queue (backlog 0 holds one) and never accept. */
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons((uint16_t)atoi(port));
    for (int i = 0; i < 4; i++)
    {
        fill[i] = socket(AF_INET, SOCK_STREAM, 0);
        fcntl(fill[i], F_SETFL, O_NONBLOCK);
        (void)connect(fill[i], (struct sockaddr *)&a, sizeof(a));
    }
    usleep(100 * 1000);

    uint64_t t0 = net_probe_now_ms();
    net_probe_result_t r = net_probe_host("127.0.0.1", port, 400);
    uint64_t took = net_probe_now_ms() - t0;

    TEST_ASSERT_EQUAL_INT(NET_PROBE_SILENT, r);
    TEST_ASSERT_TRUE_MESSAGE(took >= 350 && took < 1000, "probe did not keep its deadline");
    for (int i = 0; i < 4; i++)
        close(fill[i]);
    close(s);
#else
    TEST_IGNORE_MESSAGE("a full accept queue drops SYNs only on Linux");
#endif
}

void test_unresolvable_or_empty_host_has_no_opinion(void)
{
    TEST_ASSERT_EQUAL_INT(NET_PROBE_UNKNOWN, net_probe_host("", "4532", 100));
    TEST_ASSERT_EQUAL_INT(NET_PROBE_UNKNOWN, net_probe_host(NULL, "4532", 100));
    TEST_ASSERT_EQUAL_INT(NET_PROBE_UNKNOWN, net_probe_host("host.invalid", "4532", 100));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_listening_host_answers);
    RUN_TEST(test_refused_port_is_a_live_host);
    RUN_TEST(test_silent_host_is_reported_by_the_deadline);
    RUN_TEST(test_unresolvable_or_empty_host_has_no_opinion);
    return UNITY_END();
}
