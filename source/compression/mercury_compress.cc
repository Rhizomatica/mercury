/*
 * Mercury block compression — adaptive dual-algorithm (PPMd8 + zstd).
 *
 * TX: entropy test → try PPMd/zstd → pick smallest (including raw).
 * RX: parse header → decompress with indicated algorithm.
 *
 * Streaming mode: PPMd model carries across batches (skip Ppmd8_Init on
 * warm model), zstd uses ZSTD_CCtx_refPrefix/ZSTD_DCtx_refPrefix with
 * a 32KB sliding window of previous raw data. CRC16 in streaming header
 * detects model desync; any failure triggers streaming_reset().
 *
 * PPMd8 from LZMA SDK (Igor Pavlov, public domain).
 * zstd from Facebook (BSD-3-Clause).
 */

#include "compression/mercury_compress.h"
#include <cstdio>
#include <cstdlib>
#include <cmath>

// ---------- PPMd8 ----------
extern "C" {
#include "ppmd/Ppmd8.h"
}

// ---------- zstd ----------
#include "zstd/zstd.h"

// PPMd model order (2-16). Order 6 = 2 MB memory, good for small blocks.
#define PPMD_ORDER   6
#define PPMD_MEM_SIZE (1 << 21)  // 2 MB

// ---------- CRC16-MODBUS for streaming desync detection ----------

static uint16_t compress_crc16(const unsigned char* data, int len)
{
	uint16_t crc = 0xFFFF;
	for (int j = 0; j < len; j++)
	{
		crc ^= data[j];
		for (int i = 0; i < 8; i++)
			crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : crc >> 1;
	}
	return crc;
}

// ---------- PPMd allocator (uses malloc/free) ----------

static void* SzAlloc(ISzAllocPtr p, size_t size)
{
	(void)p;
	return malloc(size);
}

static void SzFree(ISzAllocPtr p, void* address)
{
	(void)p;
	free(address);
}

static const ISzAlloc g_Alloc = { SzAlloc, SzFree };

// ---------- PPMd byte-stream adapters (memory buffers) ----------

struct CByteOutBuf
{
	IByteOut vt;
	unsigned char* buf;
	int capacity;
	int pos;
	int overflow;
};

static void ByteOutBuf_Write(const IByteOut* pp, Byte b)
{
	// The IByteOut interface uses const pointer; cast to access our mutable struct.
	CByteOutBuf* p = (CByteOutBuf*)(void*)pp;
	if (p->pos < p->capacity)
		p->buf[p->pos++] = b;
	else
		p->overflow = 1;
}

struct CByteInBuf
{
	IByteIn vt;
	const unsigned char* buf;
	int size;
	int pos;
};

static Byte ByteInBuf_Read(const IByteIn* pp)
{
	CByteInBuf* p = (CByteInBuf*)(void*)pp;
	if (p->pos < p->size)
		return p->buf[p->pos++];
	return 0;
}

// ---------- cl_compressor ----------

cl_compressor::cl_compressor()
{
	ppmd_ctx = nullptr;
	ppmd_mem = nullptr;
	zstd_cctx = nullptr;
	zstd_dctx = nullptr;
	workspace = nullptr;
	workspace_size = 0;
	initialized = false;

	streaming_active = false;
	stream_batch_count = 0;
	ppmd_model_warm = false;
	zstd_prefix = nullptr;
	zstd_prefix_len = 0;
	pending_raw = nullptr;
	pending_raw_len = 0;
	pending_raw_capacity = 0;
}

cl_compressor::~cl_compressor()
{
	deinit();
}

void cl_compressor::init()
{
	if (initialized) return;

	// PPMd8
	CPpmd8* p = (CPpmd8*)malloc(sizeof(CPpmd8));
	if (!p) return;
	Ppmd8_Construct(p);
	if (!Ppmd8_Alloc(p, PPMD_MEM_SIZE, &g_Alloc))
	{
		free(p);
		return;
	}
	ppmd_ctx = p;

	// zstd
	zstd_cctx = ZSTD_createCCtx();
	zstd_dctx = ZSTD_createDCtx();

	if (!zstd_cctx || !zstd_dctx)
	{
		deinit();
		return;
	}

	// zstd level 3: good ratio, fast on small blocks
	ZSTD_CCtx_setParameter((ZSTD_CCtx*)zstd_cctx, ZSTD_c_compressionLevel, 3);

	// Workspace for batch-level compression intermediate buffers
	workspace_size = COMPRESS_WORKSPACE_SIZE;
	workspace = (unsigned char*)malloc(workspace_size);
	if (!workspace)
	{
		deinit();
		return;
	}

	initialized = true;
	printf("[COMPRESS] Initialized: PPMd8 (order %d, %d KB) + zstd (level 3), workspace %d KB\n",
		PPMD_ORDER, PPMD_MEM_SIZE / 1024, workspace_size / 1024);
	fflush(stdout);
}

void cl_compressor::deinit()
{
	streaming_disable();

	if (ppmd_ctx)
	{
		Ppmd8_Free((CPpmd8*)ppmd_ctx, &g_Alloc);
		free(ppmd_ctx);
		ppmd_ctx = nullptr;
	}
	if (zstd_cctx)
	{
		ZSTD_freeCCtx((ZSTD_CCtx*)zstd_cctx);
		zstd_cctx = nullptr;
	}
	if (zstd_dctx)
	{
		ZSTD_freeDCtx((ZSTD_DCtx*)zstd_dctx);
		zstd_dctx = nullptr;
	}
	if (workspace)
	{
		free(workspace);
		workspace = nullptr;
		workspace_size = 0;
	}
	initialized = false;
}

// ---------- Streaming context management ----------

void cl_compressor::streaming_enable()
{
	if (!initialized) return;
	if (streaming_active) return;  // Already enabled

	zstd_prefix = (unsigned char*)malloc(ZSTD_PREFIX_CAPACITY);
	zstd_prefix_len = 0;

	pending_raw_capacity = COMPRESS_WORKSPACE_SIZE;
	pending_raw = (unsigned char*)malloc(pending_raw_capacity);
	pending_raw_len = 0;

	stream_batch_count = 0;
	ppmd_model_warm = false;
	streaming_active = true;

	printf("[STREAMING] Enabled: PPMd carry + zstd prefix (32KB)\n");
	fflush(stdout);
}

void cl_compressor::streaming_disable()
{
	if (!streaming_active) return;
	streaming_active = false;

	if (ppmd_ctx)
		Ppmd8_Init((CPpmd8*)ppmd_ctx, PPMD_ORDER, PPMD8_RESTORE_METHOD_RESTART);
	ppmd_model_warm = false;
	stream_batch_count = 0;

	free(zstd_prefix);
	zstd_prefix = nullptr;
	zstd_prefix_len = 0;

	free(pending_raw);
	pending_raw = nullptr;
	pending_raw_len = 0;
	pending_raw_capacity = 0;

	printf("[STREAMING] Disabled\n");
	fflush(stdout);
}

void cl_compressor::streaming_reset()
{
	if (!streaming_active) return;

	if (ppmd_ctx)
		Ppmd8_Init((CPpmd8*)ppmd_ctx, PPMD_ORDER, PPMD8_RESTORE_METHOD_RESTART);
	ppmd_model_warm = false;
	zstd_prefix_len = 0;
	pending_raw_len = 0;
	stream_batch_count = 0;

	printf("[STREAMING] Reset\n");
	fflush(stdout);
}

void cl_compressor::set_pending_raw(const unsigned char* data, int len)
{
	if (!streaming_active || !pending_raw || len <= 0) return;
	if (len > pending_raw_capacity) len = pending_raw_capacity;
	memcpy(pending_raw, data, len);
	pending_raw_len = len;
}

void cl_compressor::commit_pending()
{
	if (!streaming_active || pending_raw_len <= 0) return;
	streaming_commit(pending_raw, pending_raw_len);
	pending_raw_len = 0;
}

void cl_compressor::clear_pending()
{
	pending_raw_len = 0;
}

void cl_compressor::streaming_commit(const unsigned char* raw_data, int raw_len)
{
	if (!streaming_active || !zstd_prefix || raw_len <= 0) return;

	// Append to zstd prefix (sliding window)
	if (zstd_prefix_len + raw_len <= ZSTD_PREFIX_CAPACITY)
	{
		memcpy(zstd_prefix + zstd_prefix_len, raw_data, raw_len);
		zstd_prefix_len += raw_len;
	}
	else
	{
		// Shift out old data, keep most recent
		int total_needed = zstd_prefix_len + raw_len;
		int drop = total_needed - ZSTD_PREFIX_CAPACITY;
		if (drop >= zstd_prefix_len)
		{
			// New data alone fills/exceeds buffer
			int offset = raw_len - ZSTD_PREFIX_CAPACITY;
			if (offset < 0) offset = 0;
			memcpy(zstd_prefix, raw_data + offset, raw_len - offset);
			zstd_prefix_len = raw_len - offset;
		}
		else
		{
			memmove(zstd_prefix, zstd_prefix + drop, zstd_prefix_len - drop);
			zstd_prefix_len -= drop;
			memcpy(zstd_prefix + zstd_prefix_len, raw_data, raw_len);
			zstd_prefix_len += raw_len;
		}
	}

	ppmd_model_warm = true;
	stream_batch_count++;
}

// ---------- Shannon entropy (bits per byte) ----------

float cl_compressor::quick_entropy(const unsigned char* data, int len)
{
	if (len <= 0) return 8.0f;
	int freq[256] = {0};
	for (int i = 0; i < len; i++)
		freq[data[i]]++;

	float entropy = 0.0f;
	float inv_len = 1.0f / (float)len;
	for (int i = 0; i < 256; i++)
	{
		if (freq[i] == 0) continue;
		float p = (float)freq[i] * inv_len;
		entropy -= p * log2f(p);
	}
	return entropy;
}

// ---------- PPMd compress/decompress ----------

int cl_compressor::ppmd_compress(const unsigned char* in, int in_len,
	unsigned char* out, int out_cap)
{
	if (!ppmd_ctx || in_len <= 0) return -1;

	CPpmd8* p = (CPpmd8*)ppmd_ctx;

	// Streaming: skip Init if model is warm (carries context from previous batch)
	if (!streaming_active || !ppmd_model_warm)
		Ppmd8_Init(p, PPMD_ORDER, PPMD8_RESTORE_METHOD_RESTART);

	CByteOutBuf outStream;
	outStream.vt.Write = ByteOutBuf_Write;
	outStream.buf = out;
	outStream.capacity = out_cap;
	outStream.pos = 0;
	outStream.overflow = 0;

	p->Stream.Out = &outStream.vt;
	// Range encoder init (always — resets Range/Low counters, NOT model)
	Ppmd8_Init_RangeEnc(p);

	for (int i = 0; i < in_len; i++)
		Ppmd8_EncodeSymbol(p, in[i]);

	Ppmd8_Flush_RangeEnc(p);

	if (outStream.overflow)
		return -1;
	return outStream.pos;
}

int cl_compressor::ppmd_decompress(const unsigned char* in, int in_len,
	int orig_len, unsigned char* out, int out_cap)
{
	if (!ppmd_ctx || in_len <= 0 || orig_len <= 0 || orig_len > out_cap)
		return -1;

	CPpmd8* p = (CPpmd8*)ppmd_ctx;

	// Streaming: skip Init if model is warm (carries context from previous batch)
	if (!streaming_active || !ppmd_model_warm)
		Ppmd8_Init(p, PPMD_ORDER, PPMD8_RESTORE_METHOD_RESTART);

	CByteInBuf inStream;
	inStream.vt.Read = ByteInBuf_Read;
	inStream.buf = in;
	inStream.size = in_len;
	inStream.pos = 0;

	p->Stream.In = &inStream.vt;
	if (!Ppmd8_Init_RangeDec(p))
		return -1;

	for (int i = 0; i < orig_len; i++)
	{
		int sym = Ppmd8_DecodeSymbol(p);
		if (sym < 0)
			return -1;
		out[i] = (unsigned char)sym;
	}

	return orig_len;
}

// ---------- zstd compress/decompress ----------

int cl_compressor::zstd_compress_buf(const unsigned char* in, int in_len,
	unsigned char* out, int out_cap)
{
	if (!zstd_cctx || in_len <= 0) return -1;

	// Streaming: apply prefix (one-shot, consumed on next compress call)
	if (streaming_active && zstd_prefix && zstd_prefix_len > 0)
		ZSTD_CCtx_refPrefix((ZSTD_CCtx*)zstd_cctx, zstd_prefix, zstd_prefix_len);

	size_t result = ZSTD_compress2((ZSTD_CCtx*)zstd_cctx, out, out_cap, in, in_len);
	if (ZSTD_isError(result))
		return -1;
	return (int)result;
}

int cl_compressor::zstd_decompress_buf(const unsigned char* in, int in_len,
	unsigned char* out, int out_cap)
{
	if (!zstd_dctx || in_len <= 0) return -1;

	// Streaming: apply prefix (one-shot, consumed on next decompress call)
	if (streaming_active && zstd_prefix && zstd_prefix_len > 0)
		ZSTD_DCtx_refPrefix((ZSTD_DCtx*)zstd_dctx, zstd_prefix, zstd_prefix_len);

	size_t result = ZSTD_decompressDCtx((ZSTD_DCtx*)zstd_dctx, out, out_cap, in, in_len);
	if (ZSTD_isError(result))
		return -1;
	return (int)result;
}

// ---------- Block compress (TX) ----------

int cl_compressor::compress_block(const char* in, int in_len, char* out, int out_capacity)
{
	if (!initialized || in_len <= 0 || !workspace)
		return 0;

	const unsigned char* uin = (const unsigned char*)in;
	unsigned char* uout = (unsigned char*)out;
	int hdr_size = get_header_size();

	// Quick entropy test
	float entropy = quick_entropy(uin, in_len);

	int best_algo = COMPRESS_ALGO_RAW;
	int best_comp_size = in_len;
	int best_offset = 0;  // offset into workspace where best payload lives
	bool best_is_raw = true;

	// Use heap workspace split in two halves for zstd and PPMd output
	int half = workspace_size / 2;

	// Streaming mode with warm model: PPMd only.
	// Must NOT try both algorithms — trying PPMd advances the model state.
	// If zstd were chosen, the RX side would decompress with zstd (no PPMd
	// model advancement) → model desync on next streaming batch.
	if (streaming_active && ppmd_model_warm)
	{
		int ps = ppmd_compress(uin, in_len, workspace + half, half);
		if (ps > 0 && ps < best_comp_size)
		{
			best_algo = COMPRESS_ALGO_PPMD;
			best_comp_size = ps;
			best_offset = half;
			best_is_raw = false;
		}
	}
	else
	{
		// Try zstd if entropy suggests compressibility
		if (entropy <= ENTROPY_SKIP_ALL)
		{
			int zs = zstd_compress_buf(uin, in_len, workspace, half);
			if (zs > 0 && zs < best_comp_size)
			{
				best_algo = COMPRESS_ALGO_ZSTD;
				best_comp_size = zs;
				best_offset = 0;
				best_is_raw = false;
			}
		}

		// Try PPMd if entropy suggests text-like data
		if (entropy < ENTROPY_ZSTD_ONLY)
		{
			int ps = ppmd_compress(uin, in_len, workspace + half, half);
			if (ps > 0 && ps < best_comp_size)
			{
				best_algo = COMPRESS_ALGO_PPMD;
				best_comp_size = ps;
				best_offset = half;
				best_is_raw = false;
			}
		}
	}

	// Check if compressed + header fits in output
	int compressed_total = hdr_size + best_comp_size;
	int raw_total = hdr_size + in_len;

	// If compressed >= raw, prefer raw (no overhead)
	if (!best_is_raw && compressed_total >= raw_total)
	{
		best_algo = COMPRESS_ALGO_RAW;
		best_comp_size = in_len;
		best_is_raw = true;
		// Streaming: PPMd model was advanced but RX will use RAW (no model advance).
		// Reset streaming to avoid desync.
		if (streaming_active && ppmd_model_warm)
		{
			printf("[COMPRESS] Streaming: PPMd tried but raw wins — resetting\n");
			fflush(stdout);
			streaming_reset();
		}
	}

	int total = hdr_size + best_comp_size;

	// Check if result fits in output buffer
	if (total > out_capacity)
	{
		// Streaming: model was advanced but data won't be sent as-is.
		// Caller may retry with less data — reset to prevent desync.
		if (streaming_active && ppmd_model_warm)
		{
			printf("[COMPRESS] Streaming: doesn't fit — resetting\n");
			fflush(stdout);
			streaming_reset();
		}
		return -1;
	}

	// Write header: [algo_flags:1][comp_size:2 LE][orig_size:2 LE][crc16:2 LE if streaming]
	uout[0] = (unsigned char)(best_algo |
		(streaming_active && stream_batch_count > 0 ? COMPRESS_FLAG_STREAMING : 0));
	uout[1] = (unsigned char)(best_comp_size & 0xFF);
	uout[2] = (unsigned char)((best_comp_size >> 8) & 0xFF);
	uout[3] = (unsigned char)(in_len & 0xFF);
	uout[4] = (unsigned char)((in_len >> 8) & 0xFF);

	if (streaming_active)
	{
		uint16_t crc = compress_crc16(uin, in_len);
		uout[5] = (unsigned char)(crc & 0xFF);
		uout[6] = (unsigned char)((crc >> 8) & 0xFF);
	}

	// Write payload
	if (best_is_raw)
		memcpy(uout + hdr_size, uin, in_len);
	else
		memcpy(uout + hdr_size, workspace + best_offset, best_comp_size);

	if (best_algo != COMPRESS_ALGO_RAW)
	{
		printf("[COMPRESS] %d -> %d bytes (%s%s, entropy=%.1f, ratio=%.1fx, batch=%d)\n",
			in_len, total,
			best_algo == COMPRESS_ALGO_PPMD ? "PPMd" : "zstd",
			(streaming_active && stream_batch_count > 0) ? "+stream" : "",
			entropy,
			(float)in_len / (float)best_comp_size,
			stream_batch_count);
		fflush(stdout);
	}

	return total;
}

// ---------- Block decompress (RX) ----------

int cl_compressor::decompress_block(const char* in, int in_len, char* out, int out_capacity)
{
	int hdr_size = get_header_size();
	if (!initialized || in_len < hdr_size)
		return -1;

	const unsigned char* uin = (const unsigned char*)in;
	unsigned char* uout = (unsigned char*)out;

	// Parse header
	int algo = uin[0] & COMPRESS_ALGO_MASK;
	bool is_streaming_frame = (uin[0] & COMPRESS_FLAG_STREAMING) != 0;
	int comp_size = uin[1] | (uin[2] << 8);
	int orig_size = uin[3] | (uin[4] << 8);

	// Streaming desync detection: TX reset but our model is warm
	if (streaming_active && !is_streaming_frame && ppmd_model_warm)
	{
		printf("[STREAMING] TX sent fresh frame (streaming=0) but our model is warm — resetting\n");
		fflush(stdout);
		streaming_reset();
	}

	// Sanity checks
	if (comp_size < 0 || orig_size < 0 || orig_size > out_capacity)
		return -1;
	if (hdr_size + comp_size > in_len)
		return -1;

	const unsigned char* payload = uin + hdr_size;
	int result = -1;

	if (algo == COMPRESS_ALGO_RAW)
	{
		if (comp_size != orig_size)
			return -1;
		memcpy(uout, payload, orig_size);
		result = orig_size;
	}
	else if (algo == COMPRESS_ALGO_PPMD)
	{
		result = ppmd_decompress(payload, comp_size, orig_size, uout, out_capacity);
		if (result != orig_size)
		{
			printf("[DECOMPRESS] PPMd error: expected %d, got %d\n", orig_size, result);
			fflush(stdout);
			return -1;
		}
	}
	else if (algo == COMPRESS_ALGO_ZSTD)
	{
		result = zstd_decompress_buf(payload, comp_size, uout, out_capacity);
		if (result != orig_size)
		{
			printf("[DECOMPRESS] zstd error: expected %d, got %d\n", orig_size, result);
			fflush(stdout);
			return -1;
		}
	}
	else
	{
		printf("[DECOMPRESS] Unknown algo 0x%02X\n", algo);
		fflush(stdout);
		return -1;
	}

	// CRC16 verify (streaming mode only)
	if (streaming_active && result > 0)
	{
		uint16_t expected_crc = uin[5] | (uin[6] << 8);
		uint16_t actual_crc = compress_crc16(uout, result);
		if (actual_crc != expected_crc)
		{
			printf("[DECOMPRESS] CRC16 mismatch (exp=0x%04X got=0x%04X) — streaming desync, resetting\n",
				expected_crc, actual_crc);
			fflush(stdout);
			streaming_reset();
			return -1;
		}
	}

	return result;
}
