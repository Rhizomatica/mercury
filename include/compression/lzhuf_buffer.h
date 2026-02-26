/*
 * LZHUF buffer I/O wrapper for Mercury B2F unroll/reroll.
 *
 * Provides memory-buffer encode/decode on top of the ham-radio-software/lzhuf
 * library (which uses FILE*-based I/O).  Uses temp files internally since
 * MinGW lacks fmemopen; latency is negligible for HF modem payloads (<128 KB).
 *
 * Both functions operate in B2F mode:
 *   Input/output format: [CRC16:2 LE][uncompressed_size:4 LE][LZHUF bitstream]
 *
 * The LZHUF algorithm is deterministic: encoding the same plaintext always
 * produces bitwise-identical output (validated against Pat test vectors).
 */

#ifndef LZHUF_BUFFER_H
#define LZHUF_BUFFER_H

#include <cstddef>
#include <cstdint>

// Decode a B2F LZHUF payload to plaintext.
// in:       B2F-format LZHUF data ([CRC16:2][size:4 LE][bitstream])
// in_len:   byte count of input
// out:      buffer for decompressed plaintext
// out_cap:  capacity of output buffer
// out_len:  [out] actual bytes written
// Returns 0 on success, -1 on error.
int lzhuf_decode_buffer(const uint8_t* in, size_t in_len,
                        uint8_t* out, size_t out_cap,
                        size_t* out_len);

// Encode plaintext to a B2F LZHUF payload.
// in:       plaintext data
// in_len:   byte count of input
// out:      buffer for B2F-format LZHUF output
// out_cap:  capacity of output buffer
// out_len:  [out] actual bytes written (including 6-byte B2F header)
// Returns 0 on success, -1 on error.
int lzhuf_encode_buffer(const uint8_t* in, size_t in_len,
                        uint8_t* out, size_t out_cap,
                        size_t* out_len);

#endif // LZHUF_BUFFER_H
