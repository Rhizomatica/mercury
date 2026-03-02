/*
 * Mercury: A configurable open-source software-defined modem.
 * Copyright (C) 2022-2024 Fadi Jerji
 * Author: Fadi Jerji
 * Email: fadi.jerji@  <gmail.com, caisresearch.com, ieee.org>
 * ORCID: 0000-0002-2076-5831
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, version 3 of the
 * License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 */

#ifndef INC_MFSK_H_
#define INC_MFSK_H_

#include <complex>
#include <cmath>

#define MOD_MFSK 200

class cl_mfsk
{
private:

public:
	int M;           // Number of tones per stream (e.g., 16 or 32)
	int nBits;       // log2(M) = bits per stream per symbol
	int Nc;          // Total subcarriers in OFDM frame (typically 50)
	int nStreams;    // Parallel MFSK streams (1=ROBUST_0, 2=ROBUST_1/ROBUST_2)
	int tone_hop_step; // Tone hopping step for frequency diversity (coprime with M)

	static const int MAX_STREAMS = 4;
	int stream_offsets[MAX_STREAMS]; // Starting subcarrier bin for each stream

	// MFSK preamble: known tone indices for time sync
	static const int MAX_PREAMBLE_SYMB = 8;
	int preamble_tones[MAX_PREAMBLE_SYMB]; // Known tone indices per preamble symbol
	int preamble_nSymb;                     // Number of preamble symbols used

	// ACK/BREAK/HAIL pattern: known tone sequences for pattern-based signaling.
	// WB (M>=16): 8 Welch-Costas tones × 2 reps = 16 symbols, with tone hopping.
	// NB (M<=8): 32/48-element Sidelnikov sequences, no repetition, no hopping.
	// ACK = data acknowledged, BREAK = emergency downshift, HAIL = "I am Mercury" beacon.
	static const int MAX_ACK_TONES = 48;  // Max for M=4 NB (48 symbols)
	int ack_tones[MAX_ACK_TONES];
	int break_tones[MAX_ACK_TONES];
	int hail_tones[MAX_ACK_TONES];
	int ack_pattern_len;    // Base tone sequence length (8 for WB, 32/48 for NB)
	int ack_pattern_nsymb;  // Total symbols transmitted (16 for WB, 32/48 for NB)
	int ack_match_threshold;   // Min matched symbols for ACK detection
	int break_match_threshold; // Min matched symbols for BREAK detection
	int hail_match_threshold;  // Min matched symbols for undirected HAIL detection

	// Directed HAIL: 4-tone CRC suffix appended after the "I am Mercury" prefix.
	// Derived from FNV-1a hash of the target callsign (including SSID).
	// Only stations matching the suffix respond, preventing multi-station collisions.
	static const int HAIL_SUFFIX_LEN = 4;
	int hail_suffix[HAIL_SUFFIX_LEN];          // CRC-derived suffix tones
	bool hail_directed;                          // true when suffix is active
	int hail_detect_tones[MAX_ACK_TONES + HAIL_SUFFIX_LEN]; // flat expanded array for detection
	int hail_detect_nsymb;                       // total symbols (base + suffix when directed)
	int hail_detect_threshold;                   // adjusted threshold

	void set_hail_target(const char* callsign, int len);
	void clear_hail_target();

	cl_mfsk();
	~cl_mfsk();

	void init(int _M, int _Nc, int _nStreams = 1);
	void deinit();

	// Effective bits per symbol period (nBits * nStreams)
	int bits_per_symbol() const { return nBits * nStreams; }

	// Generate MFSK preamble data (tones in all streams simultaneously)
	// preamble_out: nSymb * Nc complex values
	void generate_preamble(std::complex<double>* preamble_out, int nSymb);

	// Generate ACK pattern: ack_pattern_nsymb symbols of known tones
	// pattern_out: ack_pattern_nsymb * Nc complex values
	void generate_ack_pattern(std::complex<double>* pattern_out);

	// Generate BREAK pattern: same structure as ACK but with break_tones
	void generate_break_pattern(std::complex<double>* pattern_out);

	// Generate HAIL pattern: "I am Mercury" beacon, same structure as ACK but with hail_tones
	void generate_hail_pattern(std::complex<double>* pattern_out);

	// TX: Map bits to one-hot subcarrier vectors across all streams
	// Consumes bits_per_symbol() bits per symbol period
	void mod(const int* bits_in, int total_bits,
	         std::complex<double>* symbols_out);

	// RX: Non-coherent energy detection across all streams -> soft LLRs
	// Produces bits_per_symbol() LLRs per symbol period
	void demod(const std::complex<double>* fft_in, int total_bits,
	           float* llr_out);
};

#endif
