/* WebSocket wire-format tests
 *
 * Copyright (C) 2026 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Mercury speaks RFC 6455 to browsers and to the Fyne client with its own
 * code, having dropped a vendored library that could not be combined with
 * GPLv3.  These pin the parts of that protocol a forgiving client would let
 * us get wrong for a long time: the handshake digest, the three payload
 * length encodings, masking, and the frames we must refuse.
 */

#include <stdio.h>
#include <string.h>

#include "unity.h"
#include "ws_crypto.h"
#include "ws_frame.h"
#include "ws_json.h"

void setUp(void) {}
void tearDown(void) {}

/* ---- handshake digest ---- */

static void accept_key(const char *client_key, char *out, size_t out_sz)
{
    static const char *guid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    char src[256];
    uint8_t digest[WS_SHA1_DIGEST_LEN];

    snprintf(src, sizeof(src), "%s%s", client_key, guid);
    ws_sha1(src, strlen(src), digest);
    ws_base64_encode(digest, sizeof(digest), out, out_sz);
}

/* The worked example from RFC 6455 section 1.3.  If this drifts, every
 * browser refuses the connection with no useful diagnostic. */
static void test_handshake_accept_matches_the_rfc_example(void)
{
    char out[64];

    accept_key("dGhlIHNhbXBsZSBub25jZQ==", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("s3pPLMBiTxaQ9kYGzzhZRbK+xOo=", out);
}

static void test_sha1_matches_the_rfc3174_vectors(void)
{
    uint8_t d[WS_SHA1_DIGEST_LEN];
    static const uint8_t want_abc[WS_SHA1_DIGEST_LEN] = {
        0xa9,0x99,0x3e,0x36,0x47,0x06,0x81,0x6a,0xba,0x3e,
        0x25,0x71,0x78,0x50,0xc2,0x6c,0x9c,0xd0,0xd8,0x9d };
    static const uint8_t want_empty[WS_SHA1_DIGEST_LEN] = {
        0xda,0x39,0xa3,0xee,0x5e,0x6b,0x4b,0x0d,0x32,0x55,
        0xbf,0xef,0x95,0x60,0x18,0x90,0xaf,0xd8,0x07,0x09 };
    /* 56 bytes: the message length that lands exactly on the padding
     * boundary, which is where a hand-written SHA-1 usually breaks. */
    static const char *msg56 =
        "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    static const uint8_t want_56[WS_SHA1_DIGEST_LEN] = {
        0x84,0x98,0x3e,0x44,0x1c,0x3b,0xd2,0x6e,0xba,0xae,
        0x4a,0xa1,0xf9,0x51,0x29,0xe5,0xe5,0x46,0x70,0xf1 };

    ws_sha1("abc", 3, d);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(want_abc, d, sizeof(d));

    ws_sha1("", 0, d);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(want_empty, d, sizeof(d));

    ws_sha1(msg56, strlen(msg56), d);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(want_56, d, sizeof(d));
}

static void test_base64_pads_every_remainder(void)
{
    char out[16];

    ws_base64_encode("", 0, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("", out);
    ws_base64_encode("f", 1, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("Zg==", out);
    ws_base64_encode("fo", 2, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("Zm8=", out);
    ws_base64_encode("foo", 3, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("Zm9v", out);
    ws_base64_encode("foobar", 6, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("Zm9vYmFy", out);

    /* Too small a buffer must refuse rather than truncate: a half-written
     * accept key would be a handshake that fails for no visible reason. */
    TEST_ASSERT_EQUAL_UINT(0, ws_base64_encode("foobar", 6, out, 4));
}

/* ---- outbound header encoding ---- */

static void test_header_uses_the_shortest_length_form(void)
{
    uint8_t h[WS_FRAME_MAX_HEADER];

    /* 7-bit form, up to 125. */
    TEST_ASSERT_EQUAL_UINT(2, ws_frame_header(h, WS_OP_TEXT, 125, true));
    TEST_ASSERT_EQUAL_HEX8(0x81, h[0]);
    TEST_ASSERT_EQUAL_HEX8(125, h[1]);
    /* The server never masks. */
    TEST_ASSERT_EQUAL_HEX8(0, h[1] & 0x80);

    /* 16-bit form starts at 126. */
    TEST_ASSERT_EQUAL_UINT(4, ws_frame_header(h, WS_OP_TEXT, 126, true));
    TEST_ASSERT_EQUAL_HEX8(126, h[1]);
    TEST_ASSERT_EQUAL_HEX8(0x00, h[2]);
    TEST_ASSERT_EQUAL_HEX8(0x7E, h[3]);

    /* A radio_list frame is tens of KB: still the 16-bit form. */
    TEST_ASSERT_EQUAL_UINT(4, ws_frame_header(h, WS_OP_TEXT, 45000, true));
    TEST_ASSERT_EQUAL_HEX8(126, h[1]);
    TEST_ASSERT_EQUAL_UINT(45000, (h[2] << 8) | h[3]);

    /* 64-bit form only above 65535. */
    TEST_ASSERT_EQUAL_UINT(4, ws_frame_header(h, WS_OP_BINARY, 65535, true));
    TEST_ASSERT_EQUAL_UINT(10, ws_frame_header(h, WS_OP_BINARY, 65536, true));
    TEST_ASSERT_EQUAL_HEX8(127, h[1]);
    TEST_ASSERT_EQUAL_HEX8(0x00, h[2]);   /* high bit clear */
    TEST_ASSERT_EQUAL_HEX8(0x01, h[7]);
    TEST_ASSERT_EQUAL_HEX8(0x00, h[8]);
    TEST_ASSERT_EQUAL_HEX8(0x00, h[9]);

    /* Fragmentation clears FIN but keeps the opcode. */
    ws_frame_header(h, WS_OP_BINARY, 10, false);
    TEST_ASSERT_EQUAL_HEX8(0x02, h[0]);
}

/* The spectrum frame the waterfall depends on: 8-byte header + 512 floats. */
static void test_spectrum_frame_is_the_16bit_form(void)
{
    uint8_t h[WS_FRAME_MAX_HEADER];
    size_t n = 8 + 512 * sizeof(float);

    TEST_ASSERT_EQUAL_UINT(2056, n);
    TEST_ASSERT_EQUAL_UINT(4, ws_frame_header(h, WS_OP_BINARY, n, true));
    TEST_ASSERT_EQUAL_HEX8(0x82, h[0]);
    TEST_ASSERT_EQUAL_UINT(n, (h[2] << 8) | h[3]);
}

/* ---- inbound parsing ---- */

/* Build a masked client frame the way a browser does. */
static size_t make_client_frame(uint8_t *out, int opcode, const void *payload,
                                size_t len, bool fin)
{
    static const uint8_t mask[4] = { 0xAA, 0x55, 0x12, 0xFE };
    size_t hlen = ws_frame_header(out, opcode, len, fin);
    size_t i;

    out[1] |= 0x80;                     /* MASK bit */
    memcpy(out + hlen, mask, 4);
    for (i = 0; i < len; i++)
        out[hlen + 4 + i] = ((const uint8_t *) payload)[i] ^ mask[i & 3];

    return hlen + 4 + len;
}

static void test_parses_a_masked_text_frame_and_unmasks_it(void)
{
    uint8_t buf[128];
    ws_frame_t f;
    size_t n = make_client_frame(buf, WS_OP_TEXT, "hello", 5, true);

    TEST_ASSERT_EQUAL_INT(WS_FRAME_OK, ws_frame_parse(buf, n, true, &f));
    TEST_ASSERT_TRUE(f.fin);
    TEST_ASSERT_TRUE(f.masked);
    TEST_ASSERT_EQUAL_INT(WS_OP_TEXT, f.opcode);
    TEST_ASSERT_EQUAL_UINT(5, (unsigned) f.payload_len);
    TEST_ASSERT_EQUAL_UINT(6, f.header_len);   /* 2 + 4 mask */
    TEST_ASSERT_EQUAL_UINT(n, f.total_len);

    ws_frame_unmask(buf + f.header_len, 5, f.mask);
    TEST_ASSERT_EQUAL_MEMORY("hello", buf + f.header_len, 5);
}

/* A short read must not be mistaken for a complete frame; the server would
 * then read a payload that has not arrived. */
static void test_reports_incomplete_until_the_header_is_all_there(void)
{
    uint8_t buf[128];
    ws_frame_t f;
    size_t n = make_client_frame(buf, WS_OP_BINARY, "0123456789", 10, true);
    size_t i;

    /* A masked small frame has a 6-byte header: 2 + 4 mask.  Anything less
     * than that cannot be parsed yet. */
    for (i = 0; i < 6; i++)
        TEST_ASSERT_EQUAL_INT(WS_FRAME_INCOMPLETE,
                              ws_frame_parse(buf, i, true, &f));

    TEST_ASSERT_EQUAL_INT(WS_FRAME_OK, ws_frame_parse(buf, n, true, &f));

    /* Header complete but payload short still parses: the caller compares
     * total_len against what it holds. */
    TEST_ASSERT_EQUAL_INT(WS_FRAME_OK, ws_frame_parse(buf, 6, true, &f));
    TEST_ASSERT_TRUE(f.total_len > 6);
}

static void test_rejects_an_unmasked_client_frame(void)
{
    uint8_t buf[16];
    ws_frame_t f;
    size_t hlen = ws_frame_header(buf, WS_OP_TEXT, 3, true);

    memcpy(buf + hlen, "abc", 3);

    /* RFC 6455 5.1: a client MUST mask.  An unmasked frame is a proxy
     * cache-poisoning vector, not merely unusual. */
    TEST_ASSERT_EQUAL_INT(WS_FRAME_PROTO_ERROR,
                          ws_frame_parse(buf, hlen + 3, true, &f));

    /* The same bytes are fine in the server-to-client direction. */
    TEST_ASSERT_EQUAL_INT(WS_FRAME_OK, ws_frame_parse(buf, hlen + 3, false, &f));
}

static void test_rejects_reserved_bits_and_unknown_opcodes(void)
{
    uint8_t buf[32];
    ws_frame_t f;
    size_t n;

    n = make_client_frame(buf, WS_OP_TEXT, "x", 1, true);
    buf[0] |= 0x40;                     /* RSV1, no extension negotiated */
    TEST_ASSERT_EQUAL_INT(WS_FRAME_PROTO_ERROR, ws_frame_parse(buf, n, true, &f));

    n = make_client_frame(buf, 0x3, "x", 1, true);      /* reserved data */
    TEST_ASSERT_EQUAL_INT(WS_FRAME_PROTO_ERROR, ws_frame_parse(buf, n, true, &f));

    n = make_client_frame(buf, 0xB, "x", 1, true);      /* reserved control */
    TEST_ASSERT_EQUAL_INT(WS_FRAME_PROTO_ERROR, ws_frame_parse(buf, n, true, &f));
}

static void test_rejects_oversized_or_fragmented_control_frames(void)
{
    uint8_t buf[512];
    uint8_t payload[126];
    ws_frame_t f;
    size_t n;

    memset(payload, 'p', sizeof(payload));

    /* Control frames cap at 125 bytes. */
    n = make_client_frame(buf, WS_OP_PING, payload, 126, true);
    TEST_ASSERT_EQUAL_INT(WS_FRAME_PROTO_ERROR, ws_frame_parse(buf, n, true, &f));

    n = make_client_frame(buf, WS_OP_PING, payload, 125, true);
    TEST_ASSERT_EQUAL_INT(WS_FRAME_OK, ws_frame_parse(buf, n, true, &f));

    /* ...and are never fragmented. */
    n = make_client_frame(buf, WS_OP_CLOSE, payload, 4, false);
    TEST_ASSERT_EQUAL_INT(WS_FRAME_PROTO_ERROR, ws_frame_parse(buf, n, true, &f));
}

/* A length that could have been encoded shorter is a protocol error.  Left
 * accepted, two implementations can disagree about the same bytes. */
static void test_rejects_non_minimal_length_encodings(void)
{
    uint8_t buf[32];
    ws_frame_t f;

    /* 16-bit form carrying a value that fits in 7 bits. */
    buf[0] = 0x81; buf[1] = 0x80 | 126; buf[2] = 0x00; buf[3] = 0x05;
    memset(buf + 4, 0, 4 + 5);
    TEST_ASSERT_EQUAL_INT(WS_FRAME_PROTO_ERROR, ws_frame_parse(buf, 13, true, &f));

    /* 64-bit form carrying a value that fits in 16 bits. */
    buf[0] = 0x81; buf[1] = 0x80 | 127;
    memset(buf + 2, 0, 8);
    buf[9] = 0x05;
    memset(buf + 10, 0, 4 + 5);
    TEST_ASSERT_EQUAL_INT(WS_FRAME_PROTO_ERROR, ws_frame_parse(buf, 19, true, &f));

    /* The top bit of a 64-bit length must be clear. */
    buf[0] = 0x81; buf[1] = 0x80 | 127;
    memset(buf + 2, 0, 8);
    buf[2] = 0x80;
    memset(buf + 10, 0, 4);
    TEST_ASSERT_EQUAL_INT(WS_FRAME_PROTO_ERROR, ws_frame_parse(buf, 14, true, &f));
}

static void test_unmask_is_its_own_inverse(void)
{
    static const uint8_t mask[4] = { 0x01, 0x02, 0x03, 0x04 };
    uint8_t data[37];
    uint8_t copy[37];
    size_t i;

    for (i = 0; i < sizeof(data); i++)
        data[i] = (uint8_t) (i * 7 + 3);
    memcpy(copy, data, sizeof(data));

    /* Length is deliberately not a multiple of 4: the mask index wraps. */
    ws_frame_unmask(data, sizeof(data), mask);
    TEST_ASSERT_NOT_EQUAL(0, memcmp(copy, data, sizeof(data)));
    ws_frame_unmask(data, sizeof(data), mask);
    TEST_ASSERT_EQUAL_MEMORY(copy, data, sizeof(data));
}

/* ---- the UI command parser ---- */

static void test_parses_a_ui_command(void)
{
    const char *json =
        "{\"command\":\"set_ptt_config\",\"value\":\"serial\","
        "\"value2\":\"/dev/ttyUSB0\",\"value7\":\"3\"}";
    ws_command_t cmd;

    TEST_ASSERT_EQUAL_INT(0, ws_json_parse_command(json, strlen(json), &cmd));
    TEST_ASSERT_EQUAL_STRING("set_ptt_config", cmd.command);
    TEST_ASSERT_EQUAL_STRING("serial", cmd.value);
    TEST_ASSERT_EQUAL_STRING("/dev/ttyUSB0", cmd.value2);
    TEST_ASSERT_EQUAL_STRING("3", cmd.value7);
    TEST_ASSERT_EQUAL_STRING("", cmd.value3);   /* absent stays empty */
}

static void test_refuses_a_command_without_the_command_key(void)
{
    const char *json = "{\"value\":\"serial\"}";
    ws_command_t cmd;

    TEST_ASSERT_EQUAL_INT(-1, ws_json_parse_command(json, strlen(json), &cmd));
}

/* The parser reads a length-delimited buffer, not a C string: a WebSocket
 * payload carries no NUL and may hold one in the middle. */
static void test_parser_respects_the_given_length(void)
{
    char json[64];
    ws_command_t cmd;
    size_t n;

    memcpy(json, "{\"command\":\"ok\"}\0{\"command\":\"hidden\"}", 38);
    n = 16;

    TEST_ASSERT_EQUAL_INT(0, ws_json_parse_command(json, n, &cmd));
    TEST_ASSERT_EQUAL_STRING("ok", cmd.command);
}

static void test_sanitiser_keeps_utf8_and_replaces_the_rest(void)
{
    char s[64];

    /* Valid multi-byte sequences survive: callsigns and filenames carry
     * accented characters, and mangling those is a visible bug. */
    strcpy(s, "S\xC3\xA3o Roque");
    ws_json_sanitise_utf8(s, strlen(s));
    TEST_ASSERT_EQUAL_STRING("S\xC3\xA3o Roque", s);

    /* A control character is legal UTF-8 but illegal unescaped in JSON, and
     * the browser rejects the whole frame for it. */
    strcpy(s, "a\tb\nc");
    ws_json_sanitise_utf8(s, strlen(s));
    TEST_ASSERT_EQUAL_STRING("a?b?c", s);

    /* A truncated sequence must not run off the end of the buffer. */
    memcpy(s, "ab\xC3", 4);
    ws_json_sanitise_utf8(s, 3);
    TEST_ASSERT_EQUAL_STRING("ab?", s);

    /* A lone continuation byte has no lead. */
    memcpy(s, "a\xA9- ", 5);
    ws_json_sanitise_utf8(s, 4);
    TEST_ASSERT_EQUAL_STRING("a?- ", s);
}

/* The scanner walks the object's structure.  A substring search looked
 * equivalent for the messages the UI sends and was not: it ended a string at
 * the first quote byte without honouring the backslash before it. */
static void test_values_may_contain_escaped_quotes(void)
{
    const char *json =
        "{\"command\":\"set_ptt_config\",\"value\":\"a \\\"b\\\" c\"}";
    ws_command_t cmd;

    TEST_ASSERT_EQUAL_INT(0, ws_json_parse_command(json, strlen(json), &cmd));
    TEST_ASSERT_EQUAL_STRING("set_ptt_config", cmd.command);
    TEST_ASSERT_EQUAL_STRING("a \"b\" c", cmd.value);
}

static void test_decodes_the_json_escapes_a_browser_emits(void)
{
    /* JSON.stringify escapes quotes, backslashes and control characters;
     * everything else it leaves as raw UTF-8. */
    const char *json =
        "{\"command\":\"x\",\"value\":\"tab\\there\\nline\","
        "\"value2\":\"back\\\\slash\",\"value3\":\"\\u00e3o\"}";
    ws_command_t cmd;

    TEST_ASSERT_EQUAL_INT(0, ws_json_parse_command(json, strlen(json), &cmd));
    TEST_ASSERT_EQUAL_STRING("tab\there\nline", cmd.value);
    TEST_ASSERT_EQUAL_STRING("back\\slash", cmd.value2);
    TEST_ASSERT_EQUAL_STRING("\xC3\xA3o", cmd.value3);   /* U+00E3 as UTF-8 */
}

/* A key of the same name nested inside a value must not win over the real
 * one at the top level. */
static void test_nested_object_does_not_shadow_the_top_level_key(void)
{
    const char *json =
        "{\"value\":{\"command\":\"evil\"},\"command\":\"real\"}";
    ws_command_t cmd;

    TEST_ASSERT_EQUAL_INT(0, ws_json_parse_command(json, strlen(json), &cmd));
    TEST_ASSERT_EQUAL_STRING("real", cmd.command);
}

static void test_a_key_is_matched_whole(void)
{
    const char *json = "{\"xcommand\":\"no\",\"commandx\":\"no\","
                       "\"command\":\"yes\"}";
    ws_command_t cmd;

    TEST_ASSERT_EQUAL_INT(0, ws_json_parse_command(json, strlen(json), &cmd));
    TEST_ASSERT_EQUAL_STRING("yes", cmd.command);
}

static void test_malformed_json_is_refused_not_guessed(void)
{
    ws_command_t cmd;
    const char *unterminated = "{\"command\":\"abc";
    const char *not_an_object = "\"command\":\"abc\"";
    const char *empty = "{}";

    TEST_ASSERT_EQUAL_INT(-1, ws_json_parse_command(unterminated,
                                                    strlen(unterminated), &cmd));
    TEST_ASSERT_EQUAL_INT(-1, ws_json_parse_command(not_an_object,
                                                    strlen(not_an_object), &cmd));
    TEST_ASSERT_EQUAL_INT(-1, ws_json_parse_command(empty, strlen(empty), &cmd));
}

/* RFC 6455 8.1: a TEXT frame must be valid UTF-8, and a client that gets
 * anything else fails the connection.  Checking continuation-byte shape alone
 * let these through -- they all have well-formed tail bytes. */
static void test_sanitiser_rejects_the_invalid_utf8_that_looks_well_formed(void)
{
    char s[16];

    /* Overlong: '/' encoded in two bytes. */
    memcpy(s, "\xC0\xAF", 3);
    ws_json_sanitise_utf8(s, 2);
    TEST_ASSERT_EQUAL_STRING("??", s);

    /* Overlong NUL in three bytes. */
    memcpy(s, "\xE0\x80\x80", 4);
    ws_json_sanitise_utf8(s, 3);
    TEST_ASSERT_EQUAL_STRING("???", s);

    /* A UTF-16 surrogate has no business in UTF-8. */
    memcpy(s, "\xED\xA0\x80", 4);
    ws_json_sanitise_utf8(s, 3);
    TEST_ASSERT_EQUAL_STRING("???", s);

    /* Above U+10FFFF. */
    memcpy(s, "\xF4\x90\x80\x80", 5);
    ws_json_sanitise_utf8(s, 4);
    TEST_ASSERT_EQUAL_STRING("????", s);

    /* C0/C1 can only ever start an overlong form. */
    memcpy(s, "\xC1\xBF", 3);
    ws_json_sanitise_utf8(s, 2);
    TEST_ASSERT_EQUAL_STRING("??", s);
}

static void test_sanitiser_keeps_every_valid_length(void)
{
    char s[16];

    memcpy(s, "\xC3\xA3", 3);                   /* U+00E3 */
    ws_json_sanitise_utf8(s, 2);
    TEST_ASSERT_EQUAL_STRING("\xC3\xA3", s);

    memcpy(s, "\xE2\x82\xAC", 4);              /* U+20AC euro */
    ws_json_sanitise_utf8(s, 3);
    TEST_ASSERT_EQUAL_STRING("\xE2\x82\xAC", s);

    memcpy(s, "\xF0\x9F\x98\x80", 5);         /* U+1F600 */
    ws_json_sanitise_utf8(s, 4);
    TEST_ASSERT_EQUAL_STRING("\xF0\x9F\x98\x80", s);

    /* The boundaries of the restricted ranges are legal. */
    memcpy(s, "\xED\x9F\xBF", 4);              /* U+D7FF, just below D800 */
    ws_json_sanitise_utf8(s, 3);
    TEST_ASSERT_EQUAL_STRING("\xED\x9F\xBF", s);

    memcpy(s, "\xF4\x8F\xBF\xBF", 5);         /* U+10FFFF, the last one */
    ws_json_sanitise_utf8(s, 4);
    TEST_ASSERT_EQUAL_STRING("\xF4\x8F\xBF\xBF", s);
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_handshake_accept_matches_the_rfc_example);
    RUN_TEST(test_sha1_matches_the_rfc3174_vectors);
    RUN_TEST(test_base64_pads_every_remainder);

    RUN_TEST(test_header_uses_the_shortest_length_form);
    RUN_TEST(test_spectrum_frame_is_the_16bit_form);

    RUN_TEST(test_parses_a_masked_text_frame_and_unmasks_it);
    RUN_TEST(test_reports_incomplete_until_the_header_is_all_there);
    RUN_TEST(test_rejects_an_unmasked_client_frame);
    RUN_TEST(test_rejects_reserved_bits_and_unknown_opcodes);
    RUN_TEST(test_rejects_oversized_or_fragmented_control_frames);
    RUN_TEST(test_rejects_non_minimal_length_encodings);
    RUN_TEST(test_unmask_is_its_own_inverse);

    RUN_TEST(test_parses_a_ui_command);
    RUN_TEST(test_refuses_a_command_without_the_command_key);
    RUN_TEST(test_parser_respects_the_given_length);
    RUN_TEST(test_sanitiser_keeps_utf8_and_replaces_the_rest);
    RUN_TEST(test_values_may_contain_escaped_quotes);
    RUN_TEST(test_decodes_the_json_escapes_a_browser_emits);
    RUN_TEST(test_nested_object_does_not_shadow_the_top_level_key);
    RUN_TEST(test_a_key_is_matched_whole);
    RUN_TEST(test_malformed_json_is_refused_not_guessed);
    RUN_TEST(test_sanitiser_rejects_the_invalid_utf8_that_looks_well_formed);
    RUN_TEST(test_sanitiser_keeps_every_valid_length);

    return UNITY_END();
}
