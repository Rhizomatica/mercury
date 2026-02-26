/*
 * B2F (Bin2Forwarding) protocol handler for Mercury modem.
 *
 * Detects Winlink B2F sessions on the data port and intercepts LZHUF
 * compressed payloads.  On TX: decompresses LZHUF to plaintext so Mercury's
 * compressor (zstd/PPMd) can achieve better ratios.  On RX: recompresses
 * plaintext back to LZHUF so the local Winlink client receives valid B2F.
 *
 * The B2F framing (SID, FC proposals, FS responses, checksums) passes
 * through unchanged.  Only the payload blobs are modified.
 *
 * Architecture:
 *   Winlink client <--TCP--> [B2F handler] <--FIFO--> ARQ/compression/RF
 *
 * TX path (client -> RF):
 *   filter_tx() sits between tcp_socket_data.receive() and fifo_buffer_tx.push().
 *   It parses the B2F stream, identifies LZHUF payloads, decompresses them,
 *   and passes plaintext to the FIFO instead.
 *
 * RX path (RF -> client):
 *   filter_rx() sits between fifo_buffer_rx.pop() and tcp_socket_data.transmit().
 *   It identifies where LZHUF payloads should be (based on B2F state), LZHUF-
 *   compresses the plaintext, and sends valid B2F to the Winlink client.
 *
 * Both Mercury endpoints negotiate B2F unrolling via capability bits.
 * When active, the TX side sends uncompressed_size bytes instead of
 * compressed_size bytes for each payload.  The RX side knows to expect
 * this and rerolls accordingly.
 */

#ifndef B2F_HANDLER_H
#define B2F_HANDLER_H

#include <cstdint>
#include <cstring>

// Maximum proposals per batch (B2F spec allows up to 5)
#define B2F_MAX_PROPOSALS  5

// Payload buffer size (10 MB — worst case ~6 MB per B2F_UNROLL_REROLL.md)
#define B2F_PAYLOAD_BUF_SIZE  (10 * 1024 * 1024)

// Max plaintext buffer (largest Winlink message ~120 KB)
#define B2F_PLAIN_BUF_SIZE    (256 * 1024)

// Line buffer for B2F text framing
#define B2F_LINE_BUF_SIZE     512

struct st_b2f_proposal
{
	char type;              // 'E' = EM (encapsulated), 'C' = CM (control)
	char mid[13];           // Message ID (null-terminated)
	uint32_t uncomp_size;   // Uncompressed size (from FC line)
	uint32_t comp_size;     // Compressed size including 6-byte B2F header (from FC line)
	int accepted;           // 1 = accepted (+), 0 = rejected (-), -1 = deferred (=)
};

class cl_b2f_handler
{
public:
	cl_b2f_handler();
	~cl_b2f_handler();

	// Allocate payload buffers.  Call once at startup.
	void init();

	// Free buffers.
	void deinit();

	// Reset state for a new connection.
	void reset();

	// --- TX path: Winlink client -> FIFO (outgoing to RF) ---
	// Process data received from TCP before pushing to FIFO.
	// Parses B2F framing, decompresses LZHUF payloads to plaintext.
	// Returns bytes written to out_buf, or -1 on error.
	// If unroll is disabled, copies input to output unchanged.
	int filter_tx(const char* in, int in_len, char* out, int out_cap);

	// --- RX path: FIFO -> Winlink client (incoming from RF) ---
	// Process data from FIFO before sending to TCP.
	// Recompresses plaintext back to LZHUF where expected by B2F state.
	// Returns bytes written to out_buf, or -1 on error.
	// If unroll is disabled, copies input to output unchanged.
	int filter_rx(const char* in, int in_len, char* out, int out_cap);

	// Session state queries
	bool is_b2f_session() const { return b2f_detected; }
	bool is_initialized() const { return initialized; }

	// Enable/disable unrolling (default: true).
	// When disabled, the handler still parses B2F for logging but passes
	// all data through unchanged.
	bool unroll_enabled;

private:
	// B2F protocol states (shared between TX and RX directions)
	// Prefixed to avoid collision with datalink_defines.h macros
	enum State
	{
		B2F_IDLE,               // No B2F detected — passthrough mode
		B2F_SID_EXCHANGE,       // Parsing SID lines ([callsign-B2F...])
		B2F_WAIT_PROPOSALS,     // Waiting for FC or FF (either direction)
		B2F_PARSING_FC,         // Accumulating FC proposal lines
		B2F_WAIT_FS,            // FC+F> sent, waiting for FS response
		B2F_PAYLOAD_TRANSFER,   // Binary payload bytes flowing
		B2F_CHECKSUM,           // FQ/FF checksum confirmation
	};

	// Which side is currently the proposer
	enum Proposer
	{
		PROPOSER_NONE,
		PROPOSER_LOCAL,     // Local Winlink client proposed (TX direction)
		PROPOSER_REMOTE,    // Remote side proposed (RX direction)
	};

	// State machine
	State state;
	Proposer current_proposer;
	bool b2f_detected;

	// Proposal tracking
	st_b2f_proposal proposals[B2F_MAX_PROPOSALS];
	int num_proposals;
	int current_payload_idx;    // Which accepted proposal's payload is in transit
	int payload_bytes_remaining; // Bytes left in current payload

	// Line accumulator (B2F framing is CR-delimited text)
	// Separate buffers for TX and RX since data arrives interleaved
	char tx_line_buf[B2F_LINE_BUF_SIZE];
	int tx_line_pos;
	char rx_line_buf[B2F_LINE_BUF_SIZE];
	int rx_line_pos;

	// Payload capture buffer (for LZHUF decompression on TX side)
	uint8_t* payload_buf;
	int payload_buf_pos;

	// Plaintext buffer (for decompressed data / LZHUF recompression)
	uint8_t* plain_buf;

	bool initialized;

	// Internal helpers
	bool parse_sid_line(const char* line, int len);
	bool parse_fc_line(const char* line, int len, st_b2f_proposal* prop);
	bool parse_fs_line(const char* line, int len);
	int find_next_accepted(int from);

	// Process a complete text line in the given direction
	// Returns bytes written to out_buf
	int process_tx_line(const char* line, int len, char* out, int out_cap);
	int process_rx_line(const char* line, int len, char* out, int out_cap);

	// Handle payload bytes.  Returns bytes written to out.
	// *in_consumed is set to bytes consumed from in (may be less than in_len).
	int process_tx_payload(const char* in, int in_len, char* out, int out_cap, int* in_consumed);
	int process_rx_payload(const char* in, int in_len, char* out, int out_cap, int* in_consumed);
};

#endif // B2F_HANDLER_H
