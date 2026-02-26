/*
 * B2F handler end-to-end test.
 *
 * Tests LZHUF round-trip, TX unroll, RX reroll, full two-node round-trip,
 * passthrough mode, and non-B2F passthrough.  Links against b2f_handler,
 * lzhuf_buffer, and lzhuf.
 *
 * Build (from mercury/ directory):
 *   g++ -O2 -std=c++14 -I./include \
 *       -c source/datalink_layer/b2f_handler.cc -o /tmp/b2f_handler.o
 *   g++ -O2 -std=c++14 -I./include \
 *       -c source/compression/lzhuf_buffer.cc -o /tmp/lzhuf_buffer.o
 *   gcc -O2 -DLZHUF -DB2F \
 *       -c source/compression/lzhuf/lzhuf.c -o /tmp/lzhuf.o
 *   g++ -O2 -std=c++14 -I./include \
 *       -c tools/test_b2f_handler.cc -o /tmp/test_b2f.o
 *   g++ /tmp/test_b2f.o /tmp/b2f_handler.o /tmp/lzhuf_buffer.o /tmp/lzhuf.o \
 *       -o tools/test_b2f_handler.exe
 */

#include "datalink_layer/b2f_handler.h"
#include "compression/lzhuf_buffer.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

static int tests_passed = 0;
static int tests_failed = 0;

#define CHECK(cond, msg) do { \
	if (!(cond)) { \
		printf("  FAIL: %s (line %d)\n", msg, __LINE__); \
		tests_failed++; \
		return; \
	} \
} while(0)

#define PASS(msg) do { \
	printf("  PASS: %s\n", msg); \
	tests_passed++; \
} while(0)

// Helper: append CR to a string (B2F line delimiter)
static std::string cr(const std::string& s) { return s + "\r"; }

// ---- Shared test data ----
static uint8_t lzhuf_compressed[65536];
static size_t  lzhuf_compressed_len = 0;

static const char* test_plaintext =
	"Hello, this is a test message for B2F unroll/reroll verification. "
	"The quick brown fox jumps over the lazy dog. "
	"Mercury is a configurable open-source software-defined modem. "
	"LZHUF determinism means encode(plaintext) always produces bitwise-identical output.";

// ---- Test 1: LZHUF encode/decode round-trip + determinism ----
void test_lzhuf_roundtrip()
{
	printf("\n--- Test 1: LZHUF round-trip ---\n");

	int plaintext_len = (int)strlen(test_plaintext);

	// Encode
	int rc = lzhuf_encode_buffer((const uint8_t*)test_plaintext, plaintext_len,
		lzhuf_compressed, sizeof(lzhuf_compressed), &lzhuf_compressed_len);
	CHECK(rc == 0, "lzhuf_encode_buffer should succeed");
	CHECK(lzhuf_compressed_len > 0, "compressed output should be non-empty");
	CHECK(lzhuf_compressed_len < (size_t)plaintext_len, "should compress smaller");
	printf("  Encoded: %d -> %zu bytes (%.1fx)\n",
		plaintext_len, lzhuf_compressed_len,
		(float)plaintext_len / (float)lzhuf_compressed_len);

	// Decode
	uint8_t decoded[65536];
	size_t decoded_len = 0;
	rc = lzhuf_decode_buffer(lzhuf_compressed, lzhuf_compressed_len,
		decoded, sizeof(decoded), &decoded_len);
	CHECK(rc == 0, "lzhuf_decode_buffer should succeed");
	CHECK((int)decoded_len == plaintext_len, "decoded length should match");
	CHECK(memcmp(decoded, test_plaintext, plaintext_len) == 0, "decoded data should match");

	// Determinism: re-encode should produce identical output
	uint8_t re_encoded[65536];
	size_t re_encoded_len = 0;
	rc = lzhuf_encode_buffer((const uint8_t*)test_plaintext, plaintext_len,
		re_encoded, sizeof(re_encoded), &re_encoded_len);
	CHECK(rc == 0, "re-encode should succeed");
	CHECK(re_encoded_len == lzhuf_compressed_len, "re-encoded length should match");
	CHECK(memcmp(re_encoded, lzhuf_compressed, lzhuf_compressed_len) == 0,
		"re-encoded data should be bitwise identical");

	PASS("LZHUF round-trip + determinism");
}

// ---- Test 2: B2F TX unroll (LZHUF -> plaintext) ----
void test_b2f_tx_unroll()
{
	printf("\n--- Test 2: B2F TX unroll ---\n");

	int plaintext_len = (int)strlen(test_plaintext);

	cl_b2f_handler handler;
	handler.init();
	handler.unroll_enabled = true;
	CHECK(handler.is_initialized(), "handler should be initialized");

	char out[65536];
	int out_len;

	// 1. TX: local SID
	std::string s = cr("[PAT-12.18.0-B2FHQEHX$]");
	out_len = handler.filter_tx(s.c_str(), (int)s.size(), out, sizeof(out));
	CHECK(out_len > 0, "TX SID output");
	CHECK(handler.is_b2f_session(), "B2F detected after SID");

	// 2. RX: remote SID
	s = cr("[CMS-5.0.0-B2FHQEHX$]");
	out_len = handler.filter_rx(s.c_str(), (int)s.size(), out, sizeof(out));
	CHECK(out_len > 0, "RX SID output");

	// 3. TX: FC proposal + F>
	char fc[256];
	snprintf(fc, sizeof(fc), "FC EM ABCDEF12 %d %d\rF>\r",
		plaintext_len, (int)lzhuf_compressed_len);
	out_len = handler.filter_tx(fc, (int)strlen(fc), out, sizeof(out));
	CHECK(out_len > 0, "TX FC+F> output");

	// 4. RX: FS + (remote accepts)
	s = cr("FS +");
	out_len = handler.filter_rx(s.c_str(), (int)s.size(), out, sizeof(out));
	CHECK(out_len > 0, "RX FS output");

	// 5. TX: LZHUF payload -> should be unrolled to plaintext
	out_len = handler.filter_tx((const char*)lzhuf_compressed, (int)lzhuf_compressed_len,
		out, sizeof(out));
	CHECK(out_len == plaintext_len, "unrolled length should match plaintext");
	CHECK(memcmp(out, test_plaintext, plaintext_len) == 0, "unrolled data should match");
	printf("  TX: %zu LZHUF -> %d plaintext\n", lzhuf_compressed_len, out_len);

	// 6. TX: FF
	s = cr("FF");
	out_len = handler.filter_tx(s.c_str(), (int)s.size(), out, sizeof(out));
	CHECK(out_len > 0, "TX FF output");

	handler.deinit();
	PASS("TX unroll: LZHUF -> plaintext");
}

// ---- Test 3: B2F RX reroll (plaintext -> LZHUF) ----
void test_b2f_rx_reroll()
{
	printf("\n--- Test 3: B2F RX reroll ---\n");

	int plaintext_len = (int)strlen(test_plaintext);

	cl_b2f_handler handler;
	handler.init();
	handler.unroll_enabled = true;

	char out[65536];
	int out_len;

	// SID exchange
	std::string s = cr("[PAT-12.18.0-B2FHQEHX$]");
	handler.filter_tx(s.c_str(), (int)s.size(), out, sizeof(out));
	s = cr("[CMS-5.0.0-B2FHQEHX$]");
	handler.filter_rx(s.c_str(), (int)s.size(), out, sizeof(out));

	// RX: remote proposes FC + F>
	char fc[256];
	snprintf(fc, sizeof(fc), "FC EM REMOTEID1 %d %d\rF>\r",
		plaintext_len, (int)lzhuf_compressed_len);
	out_len = handler.filter_rx(fc, (int)strlen(fc), out, sizeof(out));
	CHECK(out_len > 0, "RX FC+F> output");

	// TX: local accepts FS +
	s = cr("FS +");
	out_len = handler.filter_tx(s.c_str(), (int)s.size(), out, sizeof(out));
	CHECK(out_len > 0, "TX FS output");

	// RX: plaintext payload -> should be rerolled to LZHUF
	// (Remote Mercury unrolled LZHUF before sending, so we receive plaintext.
	//  Our RX side must reroll back to LZHUF for the local Winlink client.)
	out_len = handler.filter_rx(test_plaintext, plaintext_len, out, sizeof(out));
	CHECK(out_len > 0, "RX reroll output should be non-empty");
	CHECK((size_t)out_len == lzhuf_compressed_len, "rerolled length should match LZHUF");
	CHECK(memcmp(out, lzhuf_compressed, lzhuf_compressed_len) == 0,
		"rerolled data should be bitwise identical to original LZHUF");
	printf("  RX: %d plaintext -> %d LZHUF\n", plaintext_len, out_len);

	// RX: FF
	s = cr("FF");
	out_len = handler.filter_rx(s.c_str(), (int)s.size(), out, sizeof(out));
	CHECK(out_len > 0, "RX FF output");

	handler.deinit();
	PASS("RX reroll: plaintext -> LZHUF (bitwise match)");
}

// ---- Test 4: Full two-node round-trip ----
void test_b2f_full_roundtrip()
{
	printf("\n--- Test 4: Full round-trip (commander + responder) ---\n");

	int plaintext_len = (int)strlen(test_plaintext);

	// Two Mercury nodes: commander (cmd) and responder (rsp)
	cl_b2f_handler cmd, rsp;
	cmd.init(); cmd.unroll_enabled = true;
	rsp.init(); rsp.unroll_enabled = true;

	char out_c[65536], out_r[65536];
	int len_c, len_r;

	// 1. Commander TX: SID  (commander's Winlink client sends SID)
	std::string s = cr("[PAT-12.18.0-B2FHQEHX$]");
	len_c = cmd.filter_tx(s.c_str(), (int)s.size(), out_c, sizeof(out_c));
	CHECK(len_c > 0, "cmd TX SID");

	// 2. Responder RX: receives commander's SID  (via RF / FIFO)
	len_r = rsp.filter_rx(out_c, len_c, out_r, sizeof(out_r));
	CHECK(len_r > 0, "rsp RX SID");

	// 3. Responder TX: SID response  (responder's Winlink client sends SID)
	s = cr("[CMS-5.0.0-B2FHQEHX$]");
	len_r = rsp.filter_tx(s.c_str(), (int)s.size(), out_r, sizeof(out_r));
	CHECK(len_r > 0, "rsp TX SID");

	// 4. Commander RX: receives responder's SID
	len_c = cmd.filter_rx(out_r, len_r, out_c, sizeof(out_c));
	CHECK(len_c > 0, "cmd RX SID");

	// 5. Commander TX: FC proposal + F>  (commander's client proposes)
	char fc[256];
	snprintf(fc, sizeof(fc), "FC EM TESTMSG1 %d %d\rF>\r",
		plaintext_len, (int)lzhuf_compressed_len);
	len_c = cmd.filter_tx(fc, (int)strlen(fc), out_c, sizeof(out_c));
	CHECK(len_c > 0, "cmd TX FC+F>");

	// 6. Responder RX: receives FC+F>
	len_r = rsp.filter_rx(out_c, len_c, out_r, sizeof(out_r));
	CHECK(len_r > 0, "rsp RX FC+F>");

	// 7. Responder TX: FS + (accept)  (responder's Winlink client accepts)
	s = cr("FS +");
	len_r = rsp.filter_tx(s.c_str(), (int)s.size(), out_r, sizeof(out_r));
	CHECK(len_r > 0, "rsp TX FS");

	// 8. Commander RX: receives FS
	len_c = cmd.filter_rx(out_r, len_r, out_c, sizeof(out_c));
	CHECK(len_c > 0, "cmd RX FS");

	// 9. Commander TX: LZHUF payload -> unrolled to plaintext
	len_c = cmd.filter_tx((const char*)lzhuf_compressed, (int)lzhuf_compressed_len,
		out_c, sizeof(out_c));
	CHECK(len_c == plaintext_len, "cmd unrolled length == plaintext length");
	CHECK(memcmp(out_c, test_plaintext, plaintext_len) == 0,
		"cmd unrolled data == original plaintext");
	printf("  Commander TX: %zu LZHUF -> %d plaintext (unrolled)\n",
		lzhuf_compressed_len, len_c);

	// === Over RF: Mercury compresses plaintext (zstd/PPMd), transmits, ===
	// === responder decompresses back to plaintext.  We simulate this   ===
	// === by passing the plaintext directly from commander to responder. ===

	// 10. Responder RX: plaintext -> rerolled to LZHUF
	len_r = rsp.filter_rx(out_c, len_c, out_r, sizeof(out_r));
	CHECK(len_r > 0, "rsp rerolled output non-empty");
	CHECK((size_t)len_r == lzhuf_compressed_len, "rsp rerolled length == LZHUF length");
	CHECK(memcmp(out_r, lzhuf_compressed, lzhuf_compressed_len) == 0,
		"rsp rerolled data == original LZHUF (bitwise identical)");
	printf("  Responder RX: %d plaintext -> %d LZHUF (rerolled, match)\n",
		len_c, len_r);

	// 11. Commander TX: FF
	s = cr("FF");
	len_c = cmd.filter_tx(s.c_str(), (int)s.size(), out_c, sizeof(out_c));
	CHECK(len_c > 0, "cmd TX FF");

	// 12. Responder RX: receives FF
	len_r = rsp.filter_rx(out_c, len_c, out_r, sizeof(out_r));
	CHECK(len_r > 0, "rsp RX FF");

	cmd.deinit(); rsp.deinit();
	PASS("Full round-trip: TX unroll -> RF -> RX reroll = bitwise match");
}

// ---- Test 5: Passthrough (unroll disabled) ----
void test_b2f_passthrough()
{
	printf("\n--- Test 5: Passthrough (unroll disabled) ---\n");

	int plaintext_len = (int)strlen(test_plaintext);

	cl_b2f_handler handler;
	handler.init();
	handler.unroll_enabled = false;

	char out[65536];
	int out_len;

	// SID exchange
	std::string s = cr("[PAT-12.18.0-B2FHQEHX$]");
	handler.filter_tx(s.c_str(), (int)s.size(), out, sizeof(out));
	s = cr("[CMS-5.0.0-B2FHQEHX$]");
	handler.filter_rx(s.c_str(), (int)s.size(), out, sizeof(out));

	// FC + F> (local proposes)
	char fc[256];
	snprintf(fc, sizeof(fc), "FC EM PASSTHRU1 %d %d\rF>\r",
		plaintext_len, (int)lzhuf_compressed_len);
	handler.filter_tx(fc, (int)strlen(fc), out, sizeof(out));

	// FS + from remote
	s = cr("FS +");
	handler.filter_rx(s.c_str(), (int)s.size(), out, sizeof(out));

	// TX payload: should pass through unchanged (LZHUF in, LZHUF out)
	out_len = handler.filter_tx((const char*)lzhuf_compressed, (int)lzhuf_compressed_len,
		out, sizeof(out));
	CHECK(out_len == (int)lzhuf_compressed_len, "passthrough length should match");
	CHECK(memcmp(out, lzhuf_compressed, lzhuf_compressed_len) == 0,
		"passthrough data should be unchanged");
	printf("  Payload passed through unchanged (%d bytes)\n", out_len);

	handler.deinit();
	PASS("Passthrough: LZHUF data unchanged when unroll disabled");
}

// ---- Test 6: Non-B2F data passthrough ----
void test_non_b2f_passthrough()
{
	printf("\n--- Test 6: Non-B2F data passthrough ---\n");

	cl_b2f_handler handler;
	handler.init();
	handler.unroll_enabled = true;

	char out[65536];
	int out_len;

	// Normal data without B2F SID -> passthrough
	const char* data = "Hello world\rSome TCP data\rMore stuff\r";
	int data_len = (int)strlen(data);
	out_len = handler.filter_tx(data, data_len, out, sizeof(out));
	CHECK(out_len == data_len, "non-B2F output length should match");
	CHECK(memcmp(out, data, data_len) == 0, "non-B2F data should be unchanged");
	CHECK(!handler.is_b2f_session(), "B2F should not be detected");
	printf("  Non-B2F data passed through unchanged (%d bytes)\n", out_len);

	handler.deinit();
	PASS("Non-B2F data passthrough");
}

// ---- Test 7: Payload + control in same chunk (edge case) ----
void test_payload_control_same_chunk()
{
	printf("\n--- Test 7: Payload + control line in same chunk ---\n");

	int plaintext_len = (int)strlen(test_plaintext);

	cl_b2f_handler handler;
	handler.init();
	handler.unroll_enabled = true;

	char out[65536];
	int out_len;

	// SID exchange
	std::string s = cr("[PAT-12.18.0-B2FHQEHX$]");
	handler.filter_tx(s.c_str(), (int)s.size(), out, sizeof(out));
	s = cr("[CMS-5.0.0-B2FHQEHX$]");
	handler.filter_rx(s.c_str(), (int)s.size(), out, sizeof(out));

	// FC + F>
	char fc[256];
	snprintf(fc, sizeof(fc), "FC EM CHUNKTST1 %d %d\rF>\r",
		plaintext_len, (int)lzhuf_compressed_len);
	handler.filter_tx(fc, (int)strlen(fc), out, sizeof(out));

	// FS
	s = cr("FS +");
	handler.filter_rx(s.c_str(), (int)s.size(), out, sizeof(out));

	// Send LZHUF payload + "FF\r" concatenated in one chunk
	int combined_len = (int)lzhuf_compressed_len + 3;  // +3 for "FF\r"
	char* combined = (char*)malloc(combined_len);
	memcpy(combined, lzhuf_compressed, lzhuf_compressed_len);
	combined[lzhuf_compressed_len] = 'F';
	combined[lzhuf_compressed_len + 1] = 'F';
	combined[lzhuf_compressed_len + 2] = '\r';

	out_len = handler.filter_tx(combined, combined_len, out, sizeof(out));
	free(combined);

	// Output should be: plaintext + "FF\r"
	int expected_len = plaintext_len + 3;  // plaintext + "FF\r"
	CHECK(out_len == expected_len, "combined output should be plaintext + FF\\r");
	CHECK(memcmp(out, test_plaintext, plaintext_len) == 0, "plaintext portion should match");
	CHECK(out[plaintext_len] == 'F' && out[plaintext_len + 1] == 'F' &&
		out[plaintext_len + 2] == '\r', "FF\\r should follow plaintext");
	printf("  Combined chunk: %d LZHUF + FF\\r -> %d plaintext + FF\\r\n",
		(int)lzhuf_compressed_len, plaintext_len);

	handler.deinit();
	PASS("Payload + control in same chunk correctly separated");
}

// ---- Test 8: Multiple proposals (accept some, reject others) ----
void test_multiple_proposals()
{
	printf("\n--- Test 8: Multiple proposals (mixed accept/reject) ---\n");

	int plaintext_len = (int)strlen(test_plaintext);

	cl_b2f_handler handler;
	handler.init();
	handler.unroll_enabled = true;

	char out[65536];
	int out_len;

	// SID exchange
	std::string s = cr("[PAT-12.18.0-B2FHQEHX$]");
	handler.filter_tx(s.c_str(), (int)s.size(), out, sizeof(out));
	s = cr("[CMS-5.0.0-B2FHQEHX$]");
	handler.filter_rx(s.c_str(), (int)s.size(), out, sizeof(out));

	// TX: 3 proposals, second rejected
	char fc[512];
	snprintf(fc, sizeof(fc),
		"FC EM MSG001 %d %d\r"
		"FC EM MSG002 %d %d\r"
		"FC EM MSG003 %d %d\r"
		"F>\r",
		plaintext_len, (int)lzhuf_compressed_len,
		plaintext_len, (int)lzhuf_compressed_len,
		plaintext_len, (int)lzhuf_compressed_len);
	handler.filter_tx(fc, (int)strlen(fc), out, sizeof(out));

	// RX: FS +-+ (accept first, reject second, accept third)
	s = cr("FS +-+");
	handler.filter_rx(s.c_str(), (int)s.size(), out, sizeof(out));

	// TX: first payload (MSG001)
	out_len = handler.filter_tx((const char*)lzhuf_compressed, (int)lzhuf_compressed_len,
		out, sizeof(out));
	CHECK(out_len == plaintext_len, "first payload unrolled length should match");
	CHECK(memcmp(out, test_plaintext, plaintext_len) == 0, "first payload should match");
	printf("  MSG001: %zu LZHUF -> %d plaintext\n", lzhuf_compressed_len, out_len);

	// TX: third payload (MSG003, MSG002 was rejected so skipped)
	out_len = handler.filter_tx((const char*)lzhuf_compressed, (int)lzhuf_compressed_len,
		out, sizeof(out));
	CHECK(out_len == plaintext_len, "third payload unrolled length should match");
	CHECK(memcmp(out, test_plaintext, plaintext_len) == 0, "third payload should match");
	printf("  MSG003: %zu LZHUF -> %d plaintext\n", lzhuf_compressed_len, out_len);

	// TX: FF
	s = cr("FF");
	out_len = handler.filter_tx(s.c_str(), (int)s.size(), out, sizeof(out));
	CHECK(out_len > 0, "FF output");

	handler.deinit();
	PASS("Multiple proposals with mixed accept/reject");
}

int main()
{
	printf("=== B2F Handler E2E Test Suite ===\n");

	// Test 1 populates lzhuf_compressed for all subsequent tests
	test_lzhuf_roundtrip();

	if (lzhuf_compressed_len == 0)
	{
		printf("\nFATAL: LZHUF encode failed, cannot continue\n");
		return 1;
	}

	test_b2f_tx_unroll();
	test_b2f_rx_reroll();
	test_b2f_full_roundtrip();
	test_b2f_passthrough();
	test_non_b2f_passthrough();
	test_payload_control_same_chunk();
	test_multiple_proposals();

	printf("\n========================================\n");
	printf("Results: %d passed, %d failed\n", tests_passed, tests_failed);
	printf("========================================\n");
	return tests_failed > 0 ? 1 : 0;
}
