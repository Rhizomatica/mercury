/* host/api_host.c: how the host's chat and listen-mode calls map onto the
 * engine -- planes, refusals and result codes.  The engine side is stubbed;
 * message_store, the broadcast framing and the MODE logic have tests of
 * their own.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "unity.h"
#include "mercury_host.h"
#include "modem.h"
#include "bcast_carousel.h"
#include "bcast_status.h"
#include "bcast_rx_host.h"

/* ---- stubs ---- */
static bool   s_connected;
static int    s_arq_rc;
static char   s_arq_sent[2048];
static size_t s_arq_len;
static int    s_bcast_rc;
static char   s_bcast_sent[2048];
static size_t s_bcast_len;
static int    s_listen = 0x10;        /* a FreeDV mode number */
static int    s_set_rc;

int   freedv_modes[]      = { 0x10, 0x11, 0x12 };
char *freedv_mode_names[] = { "DATAC1", "DATAC3", "DATAC16" };
int   mercury_cli_mode_count(void) { return 3; }

bool arq_is_link_connected(void) { return s_connected; }
int  arq_submit_tcp_payload(const uint8_t *d, size_t n)
{
    memcpy(s_arq_sent, d, n); s_arq_len = n; return s_arq_rc;
}
int broadcast_send_message(const uint8_t *m, size_t n)
{
    memcpy(s_bcast_sent, m, n); s_bcast_len = n; return s_bcast_rc;
}
size_t broadcast_message_max(void) { return 123; }
size_t msg_store_since(uint64_t a, char *o, size_t c, uint64_t *l) { (void)o; (void)c; *l = a + 1; return 7; }
uint64_t msg_store_last_seq(void) { return 42; }
int  modem_get_listen_mode(void) { return s_listen; }
int  modem_set_listen_mode(int m) { if (s_set_rc == 0) s_listen = m; return s_set_rc; }
bool modem_listen_mode_switchable(int m) { return m != 0x12; }      /* "DATAC16" is not */
int  bcast_file_mode_usable(int index) { return index == 0; }      /* only "DATAC1" */

static bool s_session;
static int  s_car_rc;
static bool s_car_stopped;
static bcast_car_status_t s_car;
bool arq_session_active(void) { return s_session; }
int  bcast_carousel_send(const char *n, const uint8_t *d, size_t l, int c, char *e, size_t el)
{
    (void)n; (void)d; (void)l; (void)c; (void)e; (void)el; return s_car_rc;
}
void bcast_carousel_stop(void) { s_car_stopped = true; }
void bcast_carousel_get(bcast_car_status_t *out) { *out = s_car; }
bool bcast_carousel_sending(void) { return s_car.state == BCAST_CAR_SENDING; }
const char *bcast_car_state_name(bcast_car_state_t st) { return st == BCAST_CAR_SENDING ? "sending" : "idle"; }

static bcast_host_object_fn s_rx_obj;
static bcast_host_frame_fn  s_rx_frame;
static int s_rx_sets;
int bcast_rx_host_set(bcast_host_object_fn o, bcast_host_frame_fn f, void *u)
{
    (void)u; s_rx_obj = o; s_rx_frame = f; s_rx_sets++; return 0;
}

void setUp(void)
{
    s_connected = true; s_arq_rc = 0; s_arq_len = 0; s_bcast_rc = 0; s_bcast_len = 0;
    s_listen = 0x10; s_set_rc = 0;
    s_session = false; s_car_rc = 0; s_car_stopped = false; memset(&s_car, 0, sizeof(s_car));
}
void tearDown(void) { }

/* An arq line goes to the session's stream with the chat client's "\n"; none
 * without a link, FULL when the queue refuses it. */
void test_chat_send_arq(void)
{
    TEST_ASSERT_EQUAL_INT(MERCURY_HOST_OK, mercury_host_chat_send("arq", "hello\n"));
    TEST_ASSERT_EQUAL_size_t(6, s_arq_len);
    TEST_ASSERT_EQUAL_MEMORY("hello\n", s_arq_sent, 6);
    s_arq_rc = -1;
    TEST_ASSERT_EQUAL_INT(MERCURY_HOST_FULL, mercury_host_chat_send("arq", "x"));
    s_connected = false;
    TEST_ASSERT_EQUAL_INT(MERCURY_HOST_NO_LINK, mercury_host_chat_send("arq", "x"));
}

/* A bcast line goes as one message ending in one "\n", as the chat client's
 * (message_store keeps a line at its newline, sender and receivers alike);
 * too long is said, and the longest leaves room for the newline. */
void test_chat_send_bcast(void)
{
    TEST_ASSERT_EQUAL_INT(MERCURY_HOST_OK, mercury_host_chat_send("bcast", "cq\r\n"));
    TEST_ASSERT_EQUAL_size_t(3, s_bcast_len);
    TEST_ASSERT_EQUAL_MEMORY("cq\n", s_bcast_sent, 3);
    TEST_ASSERT_EQUAL_INT(MERCURY_HOST_OK, mercury_host_chat_send("bcast", "de PY2XYZ"));
    TEST_ASSERT_EQUAL_MEMORY("de PY2XYZ\n", s_bcast_sent, 10);
    s_bcast_rc = -2;
    TEST_ASSERT_EQUAL_INT(MERCURY_HOST_TOO_LONG, mercury_host_chat_send("bcast", "long"));
    s_bcast_rc = -1;
    TEST_ASSERT_EQUAL_INT(MERCURY_HOST_BAD, mercury_host_chat_send("bcast", "x"));
    TEST_ASSERT_EQUAL_size_t(122, mercury_host_bcast_chat_max());
}

void test_chat_send_refuses_bad_input(void)
{
    char big[1100];
    memset(big, 'a', sizeof(big) - 1); big[sizeof(big) - 1] = '\0';
    TEST_ASSERT_EQUAL_INT(MERCURY_HOST_BAD, mercury_host_chat_send("udp", "x"));
    TEST_ASSERT_EQUAL_INT(MERCURY_HOST_BAD, mercury_host_chat_send("arq", "\n"));
    TEST_ASSERT_EQUAL_INT(MERCURY_HOST_BAD, mercury_host_chat_send("arq", big));
    TEST_ASSERT_EQUAL_INT(MERCURY_HOST_BAD, mercury_host_chat_send(NULL, "x"));
}

void test_chat_since_passes_the_cursor(void)
{
    char out[16]; uint64_t last = 0;
    TEST_ASSERT_EQUAL_size_t(7, mercury_host_chat_since(9, out, sizeof(out), &last));
    TEST_ASSERT_EQUAL_UINT64(10, last);
    TEST_ASSERT_EQUAL_UINT64(42, mercury_host_chat_last_seq());
}

/* The listen mode by index, as MODE: BUSY and BAD as the modem says. */
void test_listen_mode(void)
{
    TEST_ASSERT_EQUAL_INT(3, mercury_host_mode_count());
    TEST_ASSERT_EQUAL_STRING("DATAC3", mercury_host_mode_name(1));
    TEST_ASSERT_NULL(mercury_host_mode_name(3));
    TEST_ASSERT_TRUE(mercury_host_mode_switchable(1));
    TEST_ASSERT_FALSE(mercury_host_mode_switchable(2));
    TEST_ASSERT_TRUE(mercury_host_mode_carries_files(0));
    TEST_ASSERT_FALSE(mercury_host_mode_carries_files(1));
    TEST_ASSERT_EQUAL_INT(0, mercury_host_listen_mode());
    TEST_ASSERT_EQUAL_INT(MERCURY_HOST_OK, mercury_host_set_listen_mode(1));
    TEST_ASSERT_EQUAL_INT(1, mercury_host_listen_mode());
    s_set_rc = MODEM_LISTEN_MODE_BUSY;
    TEST_ASSERT_EQUAL_INT(MERCURY_HOST_BUSY, mercury_host_set_listen_mode(0));
    s_set_rc = MODEM_LISTEN_MODE_BAD;
    TEST_ASSERT_EQUAL_INT(MERCURY_HOST_BAD, mercury_host_set_listen_mode(2));
    TEST_ASSERT_EQUAL_INT(MERCURY_HOST_BAD, mercury_host_set_listen_mode(7));
    TEST_ASSERT_EQUAL_INT(1, mercury_host_listen_mode());
}

/* The carousel: the result codes, its stats, and what holds the modem. */
void test_bcast_carousel_calls(void)
{
    char err[64];
    TEST_ASSERT_EQUAL_INT(MERCURY_HOST_OK, mercury_host_bcast_send("m.txt", (const uint8_t *)"x", 1, 2, err, sizeof(err)));
    s_car_rc = BCAST_CAROUSEL_BUSY;
    TEST_ASSERT_EQUAL_INT(MERCURY_HOST_BUSY, mercury_host_bcast_send("m.txt", (const uint8_t *)"x", 1, 2, err, sizeof(err)));
    s_car_rc = BCAST_CAROUSEL_BAD;
    TEST_ASSERT_EQUAL_INT(MERCURY_HOST_BAD, mercury_host_bcast_send("m.txt", (const uint8_t *)"x", 1, 2, err, sizeof(err)));
    mercury_host_bcast_stop();
    TEST_ASSERT_TRUE(s_car_stopped);

    TEST_ASSERT_NULL(mercury_host_busy_reason());
    s_car.state = BCAST_CAR_SENDING; s_car.cycle = 2; s_car.cycles_total = 5; s_car.frames_sent = 77;
    snprintf(s_car.name, sizeof(s_car.name), "m.txt");
    TEST_ASSERT_EQUAL_STRING("broadcast", mercury_host_busy_reason());
    mercury_host_bcast_stats_t st;
    mercury_host_bcast_stats(&st);
    TEST_ASSERT_EQUAL_STRING("sending", st.state);
    TEST_ASSERT_EQUAL_INT(2, st.cycle);
    TEST_ASSERT_EQUAL_INT(5, st.cycles_total);
    TEST_ASSERT_EQUAL_UINT64(77, st.frames_sent);
    TEST_ASSERT_EQUAL_STRING("m.txt", st.name);
    s_session = true;                                     /* ARQ comes first */
    TEST_ASSERT_EQUAL_STRING("arq", mercury_host_busy_reason());
}

/* What the broadcast plane hears reaches the host's ops with its own user
 * pointer; only the callbacks given are started; NULL ops stop it. */
static int   s_host_frames, s_host_objects;
static void *s_host_user;
static void host_frame(const uint8_t *f, size_t l, int m, float snr, void *u)
{
    (void)f; (void)l; (void)m; (void)snr; s_host_user = u; s_host_frames++;
}
static void host_object(const uint8_t *d, size_t l, const char *n, bool sealed, void *u)
{
    (void)d; (void)l; (void)n; (void)sealed; s_host_user = u; s_host_objects++;
}

void test_bcast_rx_ops(void)
{
    int me;
    mercury_host_bcast_rx_ops ops = { .on_frame = host_frame, .user = &me };
    TEST_ASSERT_EQUAL_INT(MERCURY_HOST_OK, mercury_host_set_bcast_rx(&ops));
    TEST_ASSERT_NULL(s_rx_obj);
    TEST_ASSERT_NOT_NULL(s_rx_frame);
    s_rx_frame((const uint8_t *)"f", 1, 0x10, 3.0f, NULL);
    TEST_ASSERT_EQUAL_INT(1, s_host_frames);
    TEST_ASSERT_EQUAL_PTR(&me, s_host_user);

    ops.on_object = host_object;
    TEST_ASSERT_EQUAL_INT(MERCURY_HOST_OK, mercury_host_set_bcast_rx(&ops));
    s_rx_obj((const uint8_t *)"x", 1, "a.txt", false, NULL);
    TEST_ASSERT_EQUAL_INT(1, s_host_objects);

    TEST_ASSERT_EQUAL_INT(MERCURY_HOST_OK, mercury_host_set_bcast_rx(NULL));
    TEST_ASSERT_NULL(s_rx_obj);
    TEST_ASSERT_NULL(s_rx_frame);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_chat_send_arq);
    RUN_TEST(test_chat_send_bcast);
    RUN_TEST(test_chat_send_refuses_bad_input);
    RUN_TEST(test_chat_since_passes_the_cursor);
    RUN_TEST(test_listen_mode);
    RUN_TEST(test_bcast_carousel_calls);
    RUN_TEST(test_bcast_rx_ops);
    return UNITY_END();
}
