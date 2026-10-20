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

void setUp(void)
{
    s_connected = true; s_arq_rc = 0; s_arq_len = 0; s_bcast_rc = 0; s_bcast_len = 0;
    s_listen = 0x10; s_set_rc = 0;
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

/* A bcast line goes as one message, without its newline; too long is said. */
void test_chat_send_bcast(void)
{
    TEST_ASSERT_EQUAL_INT(MERCURY_HOST_OK, mercury_host_chat_send("bcast", "cq\r\n"));
    TEST_ASSERT_EQUAL_size_t(2, s_bcast_len);
    s_bcast_rc = -2;
    TEST_ASSERT_EQUAL_INT(MERCURY_HOST_TOO_LONG, mercury_host_chat_send("bcast", "long"));
    s_bcast_rc = -1;
    TEST_ASSERT_EQUAL_INT(MERCURY_HOST_BAD, mercury_host_chat_send("bcast", "x"));
    TEST_ASSERT_EQUAL_size_t(123, mercury_host_bcast_chat_max());
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

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_chat_send_arq);
    RUN_TEST(test_chat_send_bcast);
    RUN_TEST(test_chat_send_refuses_bad_input);
    RUN_TEST(test_chat_since_passes_the_cursor);
    RUN_TEST(test_listen_mode);
    return UNITY_END();
}
