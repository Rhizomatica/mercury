/*
 * Mercury block compression — adaptive dual-algorithm (PPMd + zstd).
 *
 * Operates at **batch level**: the ARQ layer accumulates an entire batch
 * of raw data (potentially several KB), compresses it as one block, then
 * splits the compressed output across individual frames.  This gives the
 * compressor enough context for real compression ratios (3-5× on text).
 *
 * Non-streaming header (5 bytes):
 *   [algo:1][comp_size:2 LE][orig_size:2 LE][payload...]
 *
 * Streaming header (7 bytes):
 *   [algo_flags:1][comp_size:2 LE][orig_size:2 LE][crc16:2 LE][payload...]
 *
 * algo_flags bits 0-1: algorithm (0=raw, 1=ppmd, 2=zstd)
 * algo_flags bit 2:    streaming flag (1 = context from previous batch used)
 * algo_flags bits 3-7: reserved (0)
 *
 * Streaming mode carries PPMd model and zstd prefix across consecutive
 * successful batches, improving compression from ~2.2x to 3.5-5x on text.
 * CRC16 in streaming header detects model desync; any failure resets both
 * sides to fresh context (self-correcting).
 *
 * algo = 0x00 raw, 0x01 PPMd, 0x02 zstd.
 * A quick Shannon entropy test steers algorithm selection:
 *   entropy > 7.5  → skip compression (send raw)
 *   entropy > 6.0  → try zstd only
 *   entropy <= 6.0 → try both PPMd and zstd, pick smaller
 * Raw is always the fallback if compression doesn't shrink the data.
 */

#ifndef MERCURY_COMPRESS_H
#define MERCURY_COMPRESS_H

#include <cstdint>
#include <cstring>

#define COMPRESS_HEADER_SIZE         7       // Streaming header (with CRC16)
#define COMPRESS_HEADER_SIZE_LEGACY  5       // Non-streaming header (no CRC)
#define COMPRESS_ALGO_RAW            0x00
#define COMPRESS_ALGO_PPMD           0x01
#define COMPRESS_ALGO_ZSTD           0x02
#define COMPRESS_ALGO_MASK           0x03
#define COMPRESS_FLAG_STREAMING      0x04

// Entropy thresholds (bits per byte, 0.0 = constant, 8.0 = random)
#define ENTROPY_SKIP_ALL        7.5f   // Incompressible — send raw
#define ENTROPY_ZSTD_ONLY       6.0f   // Mixed — try zstd only
                                       // Below 6.0: try both PPMd and zstd

#define COMPRESS_WORKSPACE_SIZE  65536  // 64 KB workspace for intermediate buffers
#define ZSTD_PREFIX_CAPACITY     32768  // 32 KB sliding window for zstd prefix

class cl_compressor {
public:
    cl_compressor();
    ~cl_compressor();

    void init();       // Allocate PPMd model, zstd contexts, workspace
    void deinit();     // Free all contexts and workspace

    // TX: compress input block, write header+payload to output.
    // Input can be up to COMPRESS_WORKSPACE_SIZE bytes (batch-level).
    // Tries PPMd and/or zstd based on entropy, picks smallest.
    // Falls back to raw if compression doesn't help.
    // Returns total bytes written (header + payload), or 0 on error.
    // Returns -1 if raw doesn't fit either (caller must reduce input).
    int compress_block(const char* in, int in_len, char* out, int out_capacity);

    // RX: decompress a single block (header + payload already assembled).
    // Returns bytes of decompressed data written to out, or -1 on error.
    int decompress_block(const char* in, int in_len, char* out, int out_capacity);

    // Streaming context management
    void streaming_enable();
    void streaming_disable();
    void streaming_reset();
    void streaming_commit(const unsigned char* raw_data, int raw_len);
    void set_pending_raw(const unsigned char* data, int len);
    void commit_pending();     // Move pending_raw → prefix + mark warm
    void clear_pending();
    bool is_streaming() const { return streaming_active; }
    int get_header_size() const { return streaming_active ? COMPRESS_HEADER_SIZE : COMPRESS_HEADER_SIZE_LEGACY; }

private:
    float quick_entropy(const unsigned char* data, int len);
    int ppmd_compress(const unsigned char* in, int in_len, unsigned char* out, int out_cap);
    int ppmd_decompress(const unsigned char* in, int in_len, int orig_len, unsigned char* out, int out_cap);
    int zstd_compress_buf(const unsigned char* in, int in_len, unsigned char* out, int out_cap);
    int zstd_decompress_buf(const unsigned char* in, int in_len, unsigned char* out, int out_cap);

    void* ppmd_ctx;    // CPpmd8*
    void* ppmd_mem;    // PPMd allocator memory
    void* zstd_cctx;   // ZSTD_CCtx*
    void* zstd_dctx;   // ZSTD_DCtx*
    unsigned char* workspace;  // Temp buffer for PPMd/zstd intermediate output
    int workspace_size;
    bool initialized;

    // Streaming state
    bool streaming_active;
    int stream_batch_count;       // Batches since last reset (0 = next compress is fresh)
    bool ppmd_model_warm;         // PPMd model has been used at least once without reset

    unsigned char* zstd_prefix;
    int zstd_prefix_len;

    unsigned char* pending_raw;   // Raw data from last compress, waiting for ACK
    int pending_raw_len;
    int pending_raw_capacity;
};

#endif
