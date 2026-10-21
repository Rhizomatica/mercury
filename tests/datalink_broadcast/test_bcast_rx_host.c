/* The host's broadcast receiver (datalink_broadcast/bcast_rx_host.c): frames
 * from the modem's tap reach on_frame, and a file they complete reaches
 * on_object with its bytes and name, on the module's own thread; stopping
 * removes the tap.  The modem is a captured tap; ARQ is not involved.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "unity.h"
#include "bcast_rx_host.h"
#include "bcast_file.h"

_Atomic bool mercury_stop = false;
int  freedv_modes[] = { 1001 };            /* index 0: DATAC1 for bcast_file */
int  mercury_cli_mode_count(void) { return 1; }
int  modem_get_listen_mode(void) { return 1001; }

static void (*s_tap)(const uint8_t *, size_t, int, float);
void broadcast_set_rx_tap(void (*tap)(const uint8_t *, size_t, int, float)) { s_tap = tap; }

static _Atomic int s_frames, s_objects;
static uint8_t s_got[8192];
static size_t s_got_len;
static char s_got_name[64];

static void on_frame(const uint8_t *f, size_t len, int mode, float snr, void *user)
{
    (void)f; (void)len; (void)mode; (void)snr; (void)user;
    atomic_fetch_add(&s_frames, 1);
}

static void on_object(const uint8_t *data, size_t len, const char *name, bool sealed, void *user)
{
    (void)sealed; (void)user;
    if (len <= sizeof(s_got)) { memcpy(s_got, data, len); s_got_len = len; }
    snprintf(s_got_name, sizeof(s_got_name), "%s", name);
    atomic_fetch_add(&s_objects, 1);
}

void setUp(void) { atomic_store(&s_frames, 0); atomic_store(&s_objects, 0); s_got_len = 0; }
void tearDown(void) { bcast_rx_host_set(NULL, NULL, NULL); }

void test_frames_and_a_completed_file_reach_the_host(void)
{
    const size_t N = 3000;
    uint8_t *data = malloc(N);
    for (size_t i = 0; i < N; i++) data[i] = (uint8_t)(i * 31 + 7);
    char err[160];
    bcast_file_tx_t *tx = bcast_file_tx_open_mem("hello.bin", data, N, 0, 0, 0, err, sizeof(err));
    TEST_ASSERT_NOT_NULL_MESSAGE(tx, err);

    TEST_ASSERT_EQUAL_INT(0, bcast_rx_host_set(on_object, on_frame, NULL));
    TEST_ASSERT_NOT_NULL(s_tap);

    int fs = bcast_file_tx_frame_size(tx), sent = 0;
    uint8_t frame[2048];
    for (int i = 0; i < 40 && atomic_load(&s_objects) == 0; i++)
    {
        TEST_ASSERT_EQUAL_INT(fs, bcast_file_tx_next(tx, frame, sizeof(frame)));
        s_tap(frame, (size_t)fs, 1001, 5.0f);
        sent++;
        usleep(5000);
    }
    for (int i = 0; i < 200 && atomic_load(&s_objects) == 0; i++) usleep(10000);

    TEST_ASSERT_EQUAL_INT(1, atomic_load(&s_objects));
    TEST_ASSERT_EQUAL_STRING("hello.bin", s_got_name);
    TEST_ASSERT_EQUAL_size_t(N, s_got_len);
    TEST_ASSERT_EQUAL_MEMORY(data, s_got, N);
    for (int i = 0; i < 100 && atomic_load(&s_frames) < sent; i++) usleep(10000);
    TEST_ASSERT_EQUAL_INT(sent, atomic_load(&s_frames));

    bcast_rx_host_set(NULL, NULL, NULL);
    TEST_ASSERT_NULL(s_tap);                       /* stopped: the tap is gone */
    bcast_file_tx_close(tx);
    free(data);
}

/* Frames only: no file decode is attempted, every frame still arrives. */
void test_frames_only(void)
{
    TEST_ASSERT_EQUAL_INT(0, bcast_rx_host_set(NULL, on_frame, NULL));
    uint8_t f[510] = { 0x7f };
    for (int i = 0; i < 5; i++) s_tap(f, sizeof(f), 1001, 1.0f);
    for (int i = 0; i < 100 && atomic_load(&s_frames) < 5; i++) usleep(10000);
    TEST_ASSERT_EQUAL_INT(5, atomic_load(&s_frames));
    TEST_ASSERT_EQUAL_INT(0, atomic_load(&s_objects));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_frames_and_a_completed_file_reach_the_host);
    RUN_TEST(test_frames_only);
    return UNITY_END();
}
