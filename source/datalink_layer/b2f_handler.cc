/*
 * B2F protocol handler implementation.
 *
 * Parses the B2F (Bin2Forwarding) protocol used by Winlink for message
 * transfer.  Identifies LZHUF-compressed payloads within the stream and
 * (when unrolling is enabled) decompresses them on TX / recompresses on RX.
 *
 * B2F is bidirectional: FC proposals may arrive via RX while FS responses
 * go out via TX (or vice versa).  The state machine is shared between both
 * directions, so line parsers check b2f_detected broadly rather than
 * restricting to specific states.
 *
 * B2F session structure (one round):
 *   [SID exchange]
 *   FC EM <mid> <uncomp> <comp>   (proposals, CR-delimited)
 *   F>                             (end of proposals)
 *   FS +-=...                      (accept/reject per proposal)
 *   <comp_size bytes>              (LZHUF payload for each accepted)
 *   FQ / FF                        (checksum confirmation)
 */

#include "datalink_layer/b2f_handler.h"
#include "compression/lzhuf_buffer.h"
#include <cstdio>
#include <cstdlib>

// ---- Constructor / Destructor ----

cl_b2f_handler::cl_b2f_handler()
{
	payload_buf = nullptr;
	plain_buf = nullptr;
	initialized = false;
	unroll_enabled = true;
	reset();
}

cl_b2f_handler::~cl_b2f_handler()
{
	deinit();
}

void cl_b2f_handler::init()
{
	if (initialized) return;

	payload_buf = (uint8_t*)malloc(B2F_PAYLOAD_BUF_SIZE);
	plain_buf = (uint8_t*)malloc(B2F_PLAIN_BUF_SIZE);

	if (!payload_buf || !plain_buf)
	{
		deinit();
		return;
	}

	initialized = true;
	printf("[B2F] Handler initialized (payload buf %d KB, plain buf %d KB)\n",
		B2F_PAYLOAD_BUF_SIZE / 1024, B2F_PLAIN_BUF_SIZE / 1024);
	fflush(stdout);
}

void cl_b2f_handler::deinit()
{
	if (payload_buf) { free(payload_buf); payload_buf = nullptr; }
	if (plain_buf) { free(plain_buf); plain_buf = nullptr; }
	initialized = false;
}

void cl_b2f_handler::reset()
{
	state = B2F_IDLE;
	current_proposer = PROPOSER_NONE;
	b2f_detected = false;
	num_proposals = 0;
	current_payload_idx = -1;
	payload_bytes_remaining = 0;
	tx_line_pos = 0;
	rx_line_pos = 0;
	payload_buf_pos = 0;
}

// ---- B2F Line Parsers ----

bool cl_b2f_handler::parse_sid_line(const char* line, int len)
{
	if (len < 5 || line[0] != '[' || line[len-1] != ']')
		return false;
	for (int i = 0; i < len - 2; i++)
	{
		if (line[i] == 'B' && line[i+1] == '2' && line[i+2] == 'F')
			return true;
	}
	return false;
}

bool cl_b2f_handler::parse_fc_line(const char* line, int len, st_b2f_proposal* prop)
{
	if (len < 10 || line[0] != 'F' || line[1] != 'C' || line[2] != ' ')
		return false;

	if (line[3] == 'E' && line[4] == 'M')
		prop->type = 'E';
	else if (line[3] == 'C' && line[4] == 'M')
		prop->type = 'C';
	else
		return false;

	int pos = 6;

	int mid_start = pos;
	while (pos < len && line[pos] != ' ') pos++;
	int mid_len = pos - mid_start;
	if (mid_len <= 0 || mid_len > 12) return false;
	memcpy(prop->mid, line + mid_start, mid_len);
	prop->mid[mid_len] = '\0';
	pos++;

	prop->uncomp_size = 0;
	while (pos < len && line[pos] >= '0' && line[pos] <= '9')
	{
		prop->uncomp_size = prop->uncomp_size * 10 + (line[pos] - '0');
		pos++;
	}
	pos++;

	prop->comp_size = 0;
	while (pos < len && line[pos] >= '0' && line[pos] <= '9')
	{
		prop->comp_size = prop->comp_size * 10 + (line[pos] - '0');
		pos++;
	}

	prop->accepted = -1;
	return true;
}

bool cl_b2f_handler::parse_fs_line(const char* line, int len)
{
	if (len < 3 || line[0] != 'F' || line[1] != 'S' || line[2] != ' ')
		return false;

	int response_pos = 3;
	for (int i = 0; i < num_proposals && response_pos < len; i++, response_pos++)
	{
		switch (line[response_pos])
		{
			case '+': proposals[i].accepted = 1; break;
			case '-': proposals[i].accepted = 0; break;
			case '=': proposals[i].accepted = -1; break;
			default: break;
		}
	}
	return true;
}

int cl_b2f_handler::find_next_accepted(int from)
{
	for (int i = from; i < num_proposals; i++)
	{
		if (proposals[i].accepted == 1)
			return i;
	}
	return -1;
}

// Helper: start payload transfer after FS acceptance
static void start_payload_for_proposer(cl_b2f_handler* h, const char* tag);

// ---- Shared line processing (called from both TX and RX) ----
//
// B2F lines can arrive from either direction:
//   - SID: both sides send one
//   - FC + F>: sent by the proposer (could be local or remote)
//   - FS: sent by the responder (opposite direction of FC)
//   - FQ/FF: either side
//
// We process all recognized B2F lines regardless of direction, using
// b2f_detected as the gate.

int cl_b2f_handler::process_tx_line(const char* line, int len, char* out, int out_cap)
{
	if (len == 0)
		goto passthrough;

	// --- SID detection ---
	if (!b2f_detected || state == B2F_SID_EXCHANGE)
	{
		if (parse_sid_line(line, len))
		{
			b2f_detected = true;
			state = B2F_SID_EXCHANGE;
			printf("[B2F-TX] SID: %.*s\n", len, line);
			fflush(stdout);
			goto passthrough;
		}
	}

	if (!b2f_detected)
		goto passthrough;

	// After SID exchange, any non-SID line advances to WAIT_PROPOSALS
	if (state == B2F_SID_EXCHANGE)
		state = B2F_WAIT_PROPOSALS;

	// --- FC proposal (local client proposing outward) ---
	{
		st_b2f_proposal prop;
		if (parse_fc_line(line, len, &prop))
		{
			if (current_proposer != PROPOSER_LOCAL)
			{
				num_proposals = 0;
				current_proposer = PROPOSER_LOCAL;
			}
			state = B2F_PARSING_FC;
			if (num_proposals < B2F_MAX_PROPOSALS)
			{
				proposals[num_proposals++] = prop;
				printf("[B2F-TX] FC: %s %cM uncomp=%u comp=%u\n",
					prop.mid, prop.type, prop.uncomp_size, prop.comp_size);
				fflush(stdout);
			}
			goto passthrough;
		}
	}

	// --- F> (end of proposals from local) ---
	if (len >= 2 && line[0] == 'F' && line[1] == '>')
	{
		state = B2F_WAIT_FS;
		printf("[B2F-TX] F> — %d proposals from local, awaiting FS\n", num_proposals);
		fflush(stdout);
		goto passthrough;
	}

	// --- FS response (local responding to remote proposals) ---
	if (line[0] == 'F' && line[1] == 'S' && len >= 3)
	{
		if (parse_fs_line(line, len))
		{
			printf("[B2F-TX] FS sent:");
			for (int i = 0; i < num_proposals; i++)
				printf(" %c", proposals[i].accepted == 1 ? '+' :
					proposals[i].accepted == 0 ? '-' : '=');
			printf("\n");
			fflush(stdout);

			// Remote proposed, we responded — remote payloads come via RX
			if (current_proposer == PROPOSER_REMOTE)
			{
				current_payload_idx = find_next_accepted(0);
				if (current_payload_idx >= 0)
				{
					// RX side will receive plaintext (if unroll) or LZHUF (if not)
					payload_bytes_remaining = unroll_enabled ?
						proposals[current_payload_idx].uncomp_size :
						proposals[current_payload_idx].comp_size;
					state = B2F_PAYLOAD_TRANSFER;
					payload_buf_pos = 0;
					printf("[B2F] Remote payloads expected via RX (%u bytes %s)\n",
						payload_bytes_remaining,
						unroll_enabled ? "plaintext" : "LZHUF");
					fflush(stdout);
				}
				else
					state = B2F_CHECKSUM;
			}
			goto passthrough;
		}
	}

	// --- FF / FQ ---
	if (len >= 2 && line[0] == 'F' && (line[1] == 'F' || line[1] == 'Q'))
	{
		printf("[B2F-TX] %c%c\n", line[0], line[1]);
		fflush(stdout);
		state = B2F_WAIT_PROPOSALS;
		current_proposer = PROPOSER_NONE;
		goto passthrough;
	}

passthrough:
	if (len + 1 > out_cap)
		return -1;
	memcpy(out, line, len);
	out[len] = '\r';
	return len + 1;
}

int cl_b2f_handler::process_rx_line(const char* line, int len, char* out, int out_cap)
{
	if (len == 0)
		goto passthrough;

	// --- SID detection ---
	if (!b2f_detected || state == B2F_SID_EXCHANGE)
	{
		if (parse_sid_line(line, len))
		{
			b2f_detected = true;
			state = B2F_SID_EXCHANGE;
			printf("[B2F-RX] SID: %.*s\n", len, line);
			fflush(stdout);
			goto passthrough;
		}
	}

	if (!b2f_detected)
		goto passthrough;

	if (state == B2F_SID_EXCHANGE)
		state = B2F_WAIT_PROPOSALS;

	// --- FC proposal (remote proposing to us) ---
	{
		st_b2f_proposal prop;
		if (parse_fc_line(line, len, &prop))
		{
			if (current_proposer != PROPOSER_REMOTE)
			{
				num_proposals = 0;
				current_proposer = PROPOSER_REMOTE;
			}
			state = B2F_PARSING_FC;
			if (num_proposals < B2F_MAX_PROPOSALS)
			{
				proposals[num_proposals++] = prop;
				printf("[B2F-RX] FC: %s %cM uncomp=%u comp=%u\n",
					prop.mid, prop.type, prop.uncomp_size, prop.comp_size);
				fflush(stdout);
			}
			goto passthrough;
		}
	}

	// --- F> (end of proposals from remote) ---
	if (len >= 2 && line[0] == 'F' && line[1] == '>')
	{
		state = B2F_WAIT_FS;
		printf("[B2F-RX] F> — %d proposals from remote, awaiting FS\n", num_proposals);
		fflush(stdout);
		goto passthrough;
	}

	// --- FS response from remote (to our local proposals) ---
	if (line[0] == 'F' && line[1] == 'S' && len >= 3)
	{
		if (parse_fs_line(line, len))
		{
			printf("[B2F-RX] FS received:");
			for (int i = 0; i < num_proposals; i++)
				printf(" %c", proposals[i].accepted == 1 ? '+' :
					proposals[i].accepted == 0 ? '-' : '=');
			printf("\n");
			fflush(stdout);

			// We proposed, remote responded — our payloads flow via TX
			if (current_proposer == PROPOSER_LOCAL)
			{
				current_payload_idx = find_next_accepted(0);
				if (current_payload_idx >= 0)
				{
					payload_bytes_remaining = proposals[current_payload_idx].comp_size;
					state = B2F_PAYLOAD_TRANSFER;
					payload_buf_pos = 0;
					printf("[B2F] Local payloads will flow via TX (%u bytes LZHUF)\n",
						payload_bytes_remaining);
					fflush(stdout);
				}
				else
					state = B2F_CHECKSUM;
			}
			goto passthrough;
		}
	}

	// --- FF / FQ ---
	if (len >= 2 && line[0] == 'F' && (line[1] == 'F' || line[1] == 'Q'))
	{
		printf("[B2F-RX] %c%c\n", line[0], line[1]);
		fflush(stdout);
		state = B2F_WAIT_PROPOSALS;
		current_proposer = PROPOSER_NONE;
		goto passthrough;
	}

passthrough:
	if (len + 1 > out_cap)
		return -1;
	memcpy(out, line, len);
	out[len] = '\r';
	return len + 1;
}

// ---- TX payload handling ----

int cl_b2f_handler::process_tx_payload(const char* in, int in_len, char* out, int out_cap, int* in_consumed)
{
	if (current_proposer != PROPOSER_LOCAL || current_payload_idx < 0)
	{
		int copy = in_len < out_cap ? in_len : out_cap;
		memcpy(out, in, copy);
		*in_consumed = copy;
		return copy;
	}

	int out_pos = 0;
	int in_pos = 0;

	while (in_pos < in_len && payload_bytes_remaining > 0)
	{
		int chunk = in_len - in_pos;
		if (chunk > payload_bytes_remaining)
			chunk = payload_bytes_remaining;

		if (unroll_enabled && initialized)
		{
			if (payload_buf_pos + chunk <= B2F_PAYLOAD_BUF_SIZE)
			{
				memcpy(payload_buf + payload_buf_pos, in + in_pos, chunk);
				payload_buf_pos += chunk;
			}
			in_pos += chunk;
			payload_bytes_remaining -= chunk;

			if (payload_bytes_remaining == 0)
			{
				size_t plain_len = 0;
				int rc = lzhuf_decode_buffer(payload_buf, payload_buf_pos,
					plain_buf, B2F_PLAIN_BUF_SIZE, &plain_len);

				if (rc == 0 && plain_len > 0)
				{
					printf("[B2F-TX] Unrolled %s: %d LZHUF -> %zu plaintext (%.1fx)\n",
						proposals[current_payload_idx].mid,
						payload_buf_pos, plain_len,
						(float)payload_buf_pos / (float)plain_len);
					fflush(stdout);

					if (out_pos + (int)plain_len <= out_cap)
					{
						memcpy(out + out_pos, plain_buf, plain_len);
						out_pos += (int)plain_len;
					}
				}
				else
				{
					printf("[B2F-TX] LZHUF decode FAILED for %s (rc=%d), passthrough\n",
						proposals[current_payload_idx].mid, rc);
					fflush(stdout);
					if (out_pos + payload_buf_pos <= out_cap)
					{
						memcpy(out + out_pos, payload_buf, payload_buf_pos);
						out_pos += payload_buf_pos;
					}
				}

				payload_buf_pos = 0;
				current_payload_idx = find_next_accepted(current_payload_idx + 1);
				if (current_payload_idx >= 0)
				{
					payload_bytes_remaining = proposals[current_payload_idx].comp_size;
					printf("[B2F-TX] Next payload: %s (%u bytes)\n",
						proposals[current_payload_idx].mid,
						proposals[current_payload_idx].comp_size);
					fflush(stdout);
				}
				else
				{
					state = B2F_CHECKSUM;
					printf("[B2F-TX] All payloads unrolled\n");
					fflush(stdout);
				}
			}
		}
		else
		{
			if (out_pos + chunk <= out_cap)
			{
				memcpy(out + out_pos, in + in_pos, chunk);
				out_pos += chunk;
			}
			in_pos += chunk;
			payload_bytes_remaining -= chunk;

			if (payload_bytes_remaining == 0)
			{
				current_payload_idx = find_next_accepted(current_payload_idx + 1);
				if (current_payload_idx >= 0)
					payload_bytes_remaining = proposals[current_payload_idx].comp_size;
				else
					state = B2F_CHECKSUM;
			}
		}
	}

	// Return only the payload bytes consumed; remaining bytes go back to
	// the filter's line parser for proper state-machine processing.
	*in_consumed = in_pos;
	return out_pos;
}

// ---- RX payload handling ----

int cl_b2f_handler::process_rx_payload(const char* in, int in_len, char* out, int out_cap, int* in_consumed)
{
	if (current_proposer != PROPOSER_REMOTE || current_payload_idx < 0)
	{
		int copy = in_len < out_cap ? in_len : out_cap;
		memcpy(out, in, copy);
		*in_consumed = copy;
		return copy;
	}

	int out_pos = 0;
	int in_pos = 0;

	while (in_pos < in_len && payload_bytes_remaining > 0)
	{
		int chunk = in_len - in_pos;
		if (chunk > payload_bytes_remaining)
			chunk = payload_bytes_remaining;

		if (unroll_enabled && initialized)
		{
			if (payload_buf_pos + chunk <= B2F_PAYLOAD_BUF_SIZE)
			{
				memcpy(payload_buf + payload_buf_pos, in + in_pos, chunk);
				payload_buf_pos += chunk;
			}
			in_pos += chunk;
			payload_bytes_remaining -= chunk;

			if (payload_bytes_remaining == 0)
			{
				size_t lzhuf_len = 0;
				int rc = lzhuf_encode_buffer(payload_buf, payload_buf_pos,
					plain_buf, B2F_PLAIN_BUF_SIZE, &lzhuf_len);

				if (rc == 0 && lzhuf_len > 0)
				{
					if ((uint32_t)lzhuf_len == proposals[current_payload_idx].comp_size)
					{
						printf("[B2F-RX] Rerolled %s: %d plaintext -> %zu LZHUF (match)\n",
							proposals[current_payload_idx].mid,
							payload_buf_pos, lzhuf_len);
					}
					else
					{
						printf("[B2F-RX] WARNING: Rerolled %zu != declared %u for %s\n",
							lzhuf_len, proposals[current_payload_idx].comp_size,
							proposals[current_payload_idx].mid);
					}
					fflush(stdout);

					if (out_pos + (int)lzhuf_len <= out_cap)
					{
						memcpy(out + out_pos, plain_buf, lzhuf_len);
						out_pos += (int)lzhuf_len;
					}
				}
				else
				{
					printf("[B2F-RX] LZHUF encode FAILED for %s (rc=%d), passthrough\n",
						proposals[current_payload_idx].mid, rc);
					fflush(stdout);
					if (out_pos + payload_buf_pos <= out_cap)
					{
						memcpy(out + out_pos, payload_buf, payload_buf_pos);
						out_pos += payload_buf_pos;
					}
				}

				payload_buf_pos = 0;
				current_payload_idx = find_next_accepted(current_payload_idx + 1);
				if (current_payload_idx >= 0)
				{
					payload_bytes_remaining = unroll_enabled ?
						proposals[current_payload_idx].uncomp_size :
						proposals[current_payload_idx].comp_size;
					printf("[B2F-RX] Next payload: %s (%u bytes)\n",
						proposals[current_payload_idx].mid,
						payload_bytes_remaining);
					fflush(stdout);
				}
				else
				{
					state = B2F_CHECKSUM;
					printf("[B2F-RX] All payloads rerolled\n");
					fflush(stdout);
				}
			}
		}
		else
		{
			if (out_pos + chunk <= out_cap)
			{
				memcpy(out + out_pos, in + in_pos, chunk);
				out_pos += chunk;
			}
			in_pos += chunk;
			payload_bytes_remaining -= chunk;

			if (payload_bytes_remaining == 0)
			{
				current_payload_idx = find_next_accepted(current_payload_idx + 1);
				if (current_payload_idx >= 0)
					payload_bytes_remaining = proposals[current_payload_idx].comp_size;
				else
					state = B2F_CHECKSUM;
			}
		}
	}

	// Return only the payload bytes consumed; remaining bytes go back to
	// the filter's line parser for proper state-machine processing.
	*in_consumed = in_pos;
	return out_pos;
}

// ---- Top-level filters ----

int cl_b2f_handler::filter_tx(const char* in, int in_len, char* out, int out_cap)
{
	if (!initialized)
	{
		int copy = in_len < out_cap ? in_len : out_cap;
		memcpy(out, in, copy);
		return copy;
	}

	int out_pos = 0;
	int in_pos = 0;

	// Pre-detection: pass bytes through one at a time while shadow-scanning
	// for B2F SID.  When SID is detected, stop passthrough at the \r boundary
	// and fall through to the line parser for remaining bytes in this chunk.
	// (Bulk-copy passthrough would duplicate post-SID bytes: once from
	// passthrough, again from the line parser on the next call.)
	if (!b2f_detected)
	{
		for (; in_pos < in_len && !b2f_detected; in_pos++)
		{
			char c = in[in_pos];

			if (out_pos < out_cap)
				out[out_pos++] = c;

			if (c == '\r')
			{
				if (tx_line_pos > 0)
				{
					tx_line_buf[tx_line_pos] = '\0';
					if (parse_sid_line(tx_line_buf, tx_line_pos))
					{
						b2f_detected = true;
						state = B2F_SID_EXCHANGE;
						printf("[B2F-TX] SID detected: %.*s\n", tx_line_pos, tx_line_buf);
						fflush(stdout);
					}
					tx_line_pos = 0;
				}
			}
			else if (c != '\n')
			{
				if (tx_line_pos < B2F_LINE_BUF_SIZE - 1)
					tx_line_buf[tx_line_pos++] = c;
				else
					tx_line_pos = 0;
			}
		}

		if (!b2f_detected)
			return out_pos;

		// SID found mid-chunk: remaining bytes fall through to line parser
	}

	// B2F active: full line parser with state machine

	while (in_pos < in_len)
	{
		if (state == B2F_PAYLOAD_TRANSFER && current_proposer == PROPOSER_LOCAL)
		{
			int avail = in_len - in_pos;
			int consumed = 0;
			int written = process_tx_payload(in + in_pos, avail, out + out_pos, out_cap - out_pos, &consumed);
			if (written < 0) return -1;
			out_pos += written;
			in_pos += consumed;
		}
		else
		{
			char c = in[in_pos++];

			if (c == '\r')
			{
				tx_line_buf[tx_line_pos] = '\0';
				int written = process_tx_line(tx_line_buf, tx_line_pos, out + out_pos, out_cap - out_pos);
				if (written < 0) return -1;
				out_pos += written;
				tx_line_pos = 0;
			}
			else if (c != '\n')
			{
				if (tx_line_pos < B2F_LINE_BUF_SIZE - 1)
					tx_line_buf[tx_line_pos++] = c;
			}
		}
	}

	return out_pos;
}

int cl_b2f_handler::filter_rx(const char* in, int in_len, char* out, int out_cap)
{
	if (!initialized)
	{
		int copy = in_len < out_cap ? in_len : out_cap;
		memcpy(out, in, copy);
		return copy;
	}

	int out_pos = 0;
	int in_pos = 0;

	// Pre-detection: pass bytes through one at a time while shadow-scanning
	// for B2F SID.  When SID is detected, stop passthrough at the \r boundary
	// and fall through to the line parser for remaining bytes in this chunk.
	if (!b2f_detected)
	{
		for (; in_pos < in_len && !b2f_detected; in_pos++)
		{
			char c = in[in_pos];

			if (out_pos < out_cap)
				out[out_pos++] = c;

			if (c == '\r')
			{
				if (rx_line_pos > 0)
				{
					rx_line_buf[rx_line_pos] = '\0';
					if (parse_sid_line(rx_line_buf, rx_line_pos))
					{
						b2f_detected = true;
						state = B2F_SID_EXCHANGE;
						printf("[B2F-RX] SID detected: %.*s\n", rx_line_pos, rx_line_buf);
						fflush(stdout);
					}
					rx_line_pos = 0;
				}
			}
			else if (c != '\n')
			{
				if (rx_line_pos < B2F_LINE_BUF_SIZE - 1)
					rx_line_buf[rx_line_pos++] = c;
				else
					rx_line_pos = 0;
			}
		}

		if (!b2f_detected)
			return out_pos;

		// SID found mid-chunk: remaining bytes fall through to line parser
	}

	// B2F active: full line parser with state machine

	while (in_pos < in_len)
	{
		if (state == B2F_PAYLOAD_TRANSFER && current_proposer == PROPOSER_REMOTE)
		{
			int avail = in_len - in_pos;
			int consumed = 0;
			int written = process_rx_payload(in + in_pos, avail, out + out_pos, out_cap - out_pos, &consumed);
			if (written < 0) return -1;
			out_pos += written;
			in_pos += consumed;
		}
		else
		{
			char c = in[in_pos++];

			if (c == '\r')
			{
				rx_line_buf[rx_line_pos] = '\0';
				int written = process_rx_line(rx_line_buf, rx_line_pos, out + out_pos, out_cap - out_pos);
				if (written < 0) return -1;
				out_pos += written;
				rx_line_pos = 0;
			}
			else if (c != '\n')
			{
				if (rx_line_pos < B2F_LINE_BUF_SIZE - 1)
					rx_line_buf[rx_line_pos++] = c;
			}
		}
	}

	return out_pos;
}
