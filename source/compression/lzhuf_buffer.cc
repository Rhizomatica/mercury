/*
 * LZHUF buffer I/O wrapper — temp-file based implementation.
 *
 * The ham-radio-software/lzhuf library uses FILE*-path-based Encode/Decode.
 * This wrapper writes input to a temp file, calls the library, and reads
 * the output back.  On a modern system, temp file I/O for <128 KB payloads
 * takes <1 ms — negligible for an HF modem running at <6 kbps.
 *
 * Thread safety: uses PID-unique temp file names to avoid collisions
 * between instances.  B2F processing is single-threaded within each
 * process (one data-port connection at a time).
 */

#include "compression/lzhuf_buffer.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

extern "C" {
#define LZHUF 1
#define B2F   1
#include "lzhuf/lzhuf.h"
}

// Generate unique temp file paths using PID to avoid collisions.
static void get_temp_paths(char* in_path, size_t in_sz,
                           char* out_path, size_t out_sz)
{
	const char* tmp = getenv("TEMP");
	if (!tmp) tmp = getenv("TMP");
	if (!tmp) tmp = ".";
#ifdef _WIN32
	int pid = (int)GetCurrentProcessId();
#else
	int pid = (int)getpid();
#endif
	snprintf(in_path, in_sz, "%s/_mercury_lzhuf_%d_in.tmp", tmp, pid);
	snprintf(out_path, out_sz, "%s/_mercury_lzhuf_%d_out.tmp", tmp, pid);
}

int lzhuf_decode_buffer(const uint8_t* in, size_t in_len,
                        uint8_t* out, size_t out_cap,
                        size_t* out_len)
{
	if (!in || !out || !out_len || in_len < 6)
		return -1;

	char tmp_in[512], tmp_out[512];
	get_temp_paths(tmp_in, sizeof(tmp_in), tmp_out, sizeof(tmp_out));

	// Write LZHUF payload to temp input file
	FILE* f = fopen(tmp_in, "wb");
	if (!f) return -1;
	if (fwrite(in, 1, in_len, f) != in_len) {
		fclose(f);
		remove(tmp_in);
		return -1;
	}
	fclose(f);

	// Decode (B2F mode)
	struct lzhufstruct* huf = AllocStruct();
	if (!huf) {
		remove(tmp_in);
		return -1;
	}
	int rc = Decode(0, tmp_in, tmp_out, huf, 1);
	FreeStruct(huf);
	remove(tmp_in);

	if (rc != 0) {
		remove(tmp_out);
		return -1;
	}

	// Read decoded plaintext
	f = fopen(tmp_out, "rb");
	if (!f) {
		remove(tmp_out);
		return -1;
	}
	fseek(f, 0, SEEK_END);
	long sz = ftell(f);
	rewind(f);

	if (sz < 0 || (size_t)sz > out_cap) {
		fclose(f);
		remove(tmp_out);
		return -1;
	}

	*out_len = fread(out, 1, (size_t)sz, f);
	fclose(f);
	remove(tmp_out);

	return (*out_len == (size_t)sz) ? 0 : -1;
}

int lzhuf_encode_buffer(const uint8_t* in, size_t in_len,
                        uint8_t* out, size_t out_cap,
                        size_t* out_len)
{
	if (!in || !out || !out_len || in_len == 0)
		return -1;

	char tmp_in[512], tmp_out[512];
	get_temp_paths(tmp_in, sizeof(tmp_in), tmp_out, sizeof(tmp_out));

	// Write plaintext to temp input file
	FILE* f = fopen(tmp_in, "wb");
	if (!f) return -1;
	if (fwrite(in, 1, in_len, f) != in_len) {
		fclose(f);
		remove(tmp_in);
		return -1;
	}
	fclose(f);

	// Encode (B2F mode)
	struct lzhufstruct* huf = AllocStruct();
	if (!huf) {
		remove(tmp_in);
		return -1;
	}
	int rc = Encode(0, tmp_in, tmp_out, huf, 1);
	FreeStruct(huf);
	remove(tmp_in);

	if (rc != 0) {
		remove(tmp_out);
		return -1;
	}

	// Read encoded LZHUF output (B2F format: [CRC16:2][size:4][bitstream])
	f = fopen(tmp_out, "rb");
	if (!f) {
		remove(tmp_out);
		return -1;
	}
	fseek(f, 0, SEEK_END);
	long sz = ftell(f);
	rewind(f);

	if (sz < 0 || (size_t)sz > out_cap) {
		fclose(f);
		remove(tmp_out);
		return -1;
	}

	*out_len = fread(out, 1, (size_t)sz, f);
	fclose(f);
	remove(tmp_out);

	return (*out_len == (size_t)sz) ? 0 : -1;
}
