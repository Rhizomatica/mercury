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

#include "physical_layer/mfsk.h"
#include <cstdint>
#include <cstdio>

cl_mfsk::cl_mfsk()
{
	M = 0;
	nBits = 0;
	Nc = 0;
	nStreams = 0;
	tone_hop_step = 0;
	preamble_nSymb = 0;
	for (int i = 0; i < MAX_STREAMS; i++)
		stream_offsets[i] = 0;
	for (int i = 0; i < MAX_PREAMBLE_SYMB; i++)
		preamble_tones[i] = 0;
	for (int i = 0; i < MAX_ACK_TONES; i++)
		ack_tones[i] = 0;
	for (int i = 0; i < MAX_ACK_TONES; i++)
		break_tones[i] = 0;
	for (int i = 0; i < MAX_ACK_TONES; i++)
		hail_tones[i] = 0;
	for (int i = 0; i < HAIL_SUFFIX_LEN; i++)
		hail_suffix[i] = 0;
	for (int i = 0; i < MAX_ACK_TONES + HAIL_SUFFIX_LEN; i++)
		hail_detect_tones[i] = 0;
	hail_directed = false;
	hail_detect_nsymb = 0;
	hail_detect_threshold = 0;
	ack_pattern_len = 0;
	ack_pattern_nsymb = 0;
	ack_match_threshold = 0;
	break_match_threshold = 0;
	hail_match_threshold = 0;
}

cl_mfsk::~cl_mfsk()
{
	deinit();
}

void cl_mfsk::init(int _M, int _Nc, int _nStreams)
{
	M = _M;
	Nc = _Nc;
	nStreams = _nStreams;
	if (nStreams < 1) nStreams = 1;
	if (nStreams > MAX_STREAMS) nStreams = MAX_STREAMS;

	// Calculate log2(M)
	nBits = 0;
	int temp = M;
	while (temp > 1)
	{
		nBits++;
		temp >>= 1;
	}

	// Tone hop step: must be coprime with M for full-period cycling
	if (M == 32)
		tone_hop_step = 13;  // 13 is prime, coprime with 32
	else if (M == 16)
		tone_hop_step = 7;   // 7 is prime, coprime with 16
	else if (M == 8)
		tone_hop_step = 3;   // 3 is prime, coprime with 8 (narrowband)
	else if (M == 4)
		tone_hop_step = 1;   // coprime with 4 (narrowband 2-stream)
	else
		tone_hop_step = 1;

	// Stream frequency allocation: center all streams within Nc subcarriers
	// Each stream gets M contiguous bins, streams are adjacent
	int total_bins = nStreams * M;
	int global_offset = (Nc - total_bins) / 2;
	if (global_offset < 0) global_offset = 0;
	for (int k = 0; k < nStreams; k++)
		stream_offsets[k] = global_offset + k * M;

	// MFSK preamble: known tone sequence spread across each stream's band
	// Same tone index used in all streams simultaneously
	// NB (M<=8): 8-symbol preamble for cross-correlation detection
	// WB (M>=16): 4-symbol preamble for FFT energy detection
	if (M == 32)
	{
		preamble_nSymb = 4;
		preamble_tones[0] = 4;
		preamble_tones[1] = 20;
		preamble_tones[2] = 12;
		preamble_tones[3] = 28;
	}
	else if (M == 16)
	{
		preamble_nSymb = 4;
		preamble_tones[0] = 2;
		preamble_tones[1] = 10;
		preamble_tones[2] = 6;
		preamble_tones[3] = 14;
	}
	else if (M == 8)
	{
		// Narrowband: 8 symbols, all tones used once
		preamble_nSymb = 8;
		preamble_tones[0] = 1;
		preamble_tones[1] = 5;
		preamble_tones[2] = 3;
		preamble_tones[3] = 7;
		preamble_tones[4] = 0;
		preamble_tones[5] = 6;
		preamble_tones[6] = 2;
		preamble_tones[7] = 4;
	}
	else if (M == 4)
	{
		// Narrowband 2-stream: 8 symbols, palindrome for symmetry
		preamble_nSymb = 8;
		preamble_tones[0] = 0;
		preamble_tones[1] = 2;
		preamble_tones[2] = 1;
		preamble_tones[3] = 3;
		preamble_tones[4] = 3;
		preamble_tones[5] = 1;
		preamble_tones[6] = 2;
		preamble_tones[7] = 0;
	}
	else
	{
		preamble_nSymb = 4;
		// Generic: spread evenly
		for (int i = 0; i < preamble_nSymb && i < MAX_PREAMBLE_SYMB; i++)
			preamble_tones[i] = (i * M / preamble_nSymb + M / (2 * preamble_nSymb)) % M;
	}

	// ACK pattern tones.
	// WB (M>=16): Welch-Costas array (p=17, g=5), 8 base tones × 2 reps = 16 symbols.
	//   Costas property: all pairwise (dt, df) difference vectors are unique.
	//   Tone hopping provides additional frequency diversity.
	// NB (M<=8): Sidelnikov sequences — longer patterns (32/48 symbols) with optimal
	//   Hamming autocorrelation. No repetition, no tone hopping (diversity is intrinsic).
	//   Needed because small M makes short Costas arrays vulnerable to false detection
	//   (P(false/poll) = 2.7% for M=8, 50% for M=4 with 16-symbol patterns).
	if (M == 32)
	{
		// 2x scaled M=16 Costas values. Avoids preamble {4,20,12,28}.
		ack_pattern_len = 8;
		ack_pattern_nsymb = 16; // 8 × 2 reps
		ack_match_threshold = 8;
		const int tones[] = {8, 14, 10, 24, 26, 2, 18, 30};
		for (int i = 0; i < 8; i++) ack_tones[i] = tones[i];
	}
	else if (M == 16)
	{
		// Welch-Costas (p=17, g=5). Avoids preamble {2,6,10,14}.
		ack_pattern_len = 8;
		ack_pattern_nsymb = 16;
		ack_match_threshold = 8;
		const int tones[] = {4, 7, 5, 12, 13, 1, 9, 15};
		for (int i = 0; i < 8; i++) ack_tones[i] = tones[i];
	}
	else if (M == 8)
	{
		// Sidelnikov (p=37, g=2, offset=3). 32 symbols, no repetition.
		// Hamming autocorrelation: max 3 coincidences at any shift (L-G bound).
		ack_pattern_len = 32;
		ack_pattern_nsymb = 32;
		ack_match_threshold = 24; // 75% — P(false|p=0.25) ≈ 10^-11
		const int tones[] = {
			1, 3, 6, 5, 3, 7, 6, 5, 2, 5, 3, 6, 4, 1, 3, 7,
			7, 7, 6, 4, 1, 2, 4, 0, 1, 2, 5, 2, 4, 1, 3, 6
		};
		for (int i = 0; i < 32; i++) ack_tones[i] = tones[i];
	}
	else if (M == 4)
	{
		// Sidelnikov (p=53, g=2, offset=3). 48 symbols, no repetition.
		ack_pattern_len = 48;
		ack_pattern_nsymb = 48;
		ack_match_threshold = 40; // 83% — P(false|p=0.5) ≈ 10^-9
		const int tones[] = {
			0, 1, 2, 0, 1, 3, 2, 1, 2, 1, 2, 0, 1, 2, 0, 0,
			0, 1, 3, 3, 2, 0, 1, 3, 3, 3, 3, 2, 1, 3, 2, 0,
			1, 2, 1, 2, 1, 3, 2, 1, 3, 3, 3, 2, 0, 0, 1, 3
		};
		for (int i = 0; i < 48; i++) ack_tones[i] = tones[i];
	}
	else
	{
		ack_pattern_len = 8;
		ack_pattern_nsymb = 16;
		ack_match_threshold = 8;
		for (int i = 0; i < ack_pattern_len; i++)
			ack_tones[i] = (i * M / ack_pattern_len + 1) % M;
	}

	// BREAK pattern tones.
	// WB: Welch-Costas (p=17, g=7) — different generator from ACK (g=5).
	// NB: Sidelnikov with different primitive root (g=5 vs ACK g=2).
	// Cross-correlation at shift 0: M=8: 5/32, M=4: 13/48 (near random baseline).
	if (M == 32)
	{
		const int tones[] = {12, 28, 4, 6, 20, 16, 22, 30};
		for (int i = 0; i < 8; i++) break_tones[i] = tones[i];
		break_match_threshold = 8;
	}
	else if (M == 16)
	{
		const int tones[] = {6, 14, 2, 3, 10, 8, 11, 15};
		for (int i = 0; i < 8; i++) break_tones[i] = tones[i];
		break_match_threshold = 12;
	}
	else if (M == 8)
	{
		// Sidelnikov (p=37, g=5, offset=3). 32 symbols.
		const int tones[] = {
			3, 7, 3, 2, 3, 3, 1, 6, 0, 2, 2, 6, 6, 7, 4, 7,
			6, 2, 4, 0, 4, 5, 4, 4, 6, 1, 7, 5, 5, 1, 1, 0
		};
		for (int i = 0; i < 32; i++) break_tones[i] = tones[i];
		break_match_threshold = 24;
	}
	else if (M == 4)
	{
		// Sidelnikov (p=53, g=5, offset=3). 48 symbols.
		const int tones[] = {
			1, 3, 3, 3, 0, 1, 1, 0, 1, 3, 1, 0, 3, 0, 0, 0,
			2, 1, 2, 2, 2, 2, 1, 3, 3, 2, 2, 0, 0, 0, 3, 2,
			2, 3, 2, 0, 2, 3, 0, 3, 3, 3, 1, 2, 1, 1, 1, 1
		};
		for (int i = 0; i < 48; i++) break_tones[i] = tones[i];
		break_match_threshold = 40;
	}
	else
	{
		break_match_threshold = 8;
		for (int i = 0; i < ack_pattern_len; i++)
			break_tones[i] = (ack_tones[i] + M / 2) % M;
	}

	// HAIL pattern tones: "I am Mercury" beacon.
	// WB: Welch-Costas (p=17, g=6) — different generator from ACK (g=5) and BREAK (g=7).
	//   Cross-correlation: vs ACK 1/8, vs BREAK 2/8 (near random).
	// NB: Sidelnikov with different primitive roots:
	//   M=8: (p=37, g=24, offset=3) — vs ACK 8/32, vs BREAK 6/32.
	//   M=4: (p=53, g=13, offset=3) — vs ACK 10/48, vs BREAK 11/48.
	if (M == 32)
	{
		// 2x scaled M=16 Welch-Costas (g=6)
		const int tones[] = {0, 10, 2, 22, 6, 12, 14, 26};
		for (int i = 0; i < 8; i++) hail_tones[i] = tones[i];
		hail_match_threshold = 8;
	}
	else if (M == 16)
	{
		// Welch-Costas (p=17, g=6)
		const int tones[] = {0, 5, 1, 11, 3, 6, 7, 13};
		for (int i = 0; i < 8; i++) hail_tones[i] = tones[i];
		hail_match_threshold = 8;
	}
	else if (M == 8)
	{
		// Sidelnikov (p=37, g=24, offset=3). 32 symbols.
		const int tones[] = {
			4, 3, 0, 2, 5, 5, 6, 0, 4, 2, 7, 1, 5, 5, 4, 3,
			2, 7, 7, 0, 3, 1, 6, 6, 5, 3, 7, 1, 4, 2, 6, 6
		};
		for (int i = 0; i < 32; i++) hail_tones[i] = tones[i];
		hail_match_threshold = 24; // 75% — matches ACK/BREAK. Was 16: P(false)≈10^-4/pos on HF noise
	}
	else if (M == 4)
	{
		// Sidelnikov (p=53, g=13, offset=3). 48 symbols.
		const int tones[] = {
			0, 0, 1, 3, 2, 3, 1, 2, 3, 3, 1, 3, 0, 0, 0, 1,
			3, 2, 3, 1, 2, 3, 3, 1, 3, 0, 0, 0, 1, 3, 2, 3,
			1, 2, 3, 3, 1, 3, 0, 0, 0, 1, 3, 2, 3, 1, 2, 3
		};
		for (int i = 0; i < 48; i++) hail_tones[i] = tones[i];
		hail_match_threshold = 40;
	}
	else
	{
		hail_match_threshold = 8;
		for (int i = 0; i < ack_pattern_len; i++)
			hail_tones[i] = (ack_tones[i] + M / 4) % M;
	}

	// Initialize directed HAIL detect arrays (undirected by default)
	clear_hail_target();
}

void cl_mfsk::set_hail_target(const char* callsign, int len)
{
	if (!callsign || len <= 0 || M == 0)
	{
		clear_hail_target();
		return;
	}

	// FNV-1a 32-bit hash of uppercase callsign (case-insensitive matching)
	uint32_t hash = 2166136261u;
	for (int i = 0; i < len; i++)
	{
		char c = callsign[i];
		if (c >= 'a' && c <= 'z') c -= 32;  // uppercase
		hash ^= (uint8_t)c;
		hash *= 16777619u;
	}

	// Derive 4 suffix tones from hash
	for (int i = 0; i < HAIL_SUFFIX_LEN; i++)
		hail_suffix[i] = (int)((hash >> (i * 5)) & 0x1F) % M;

	hail_directed = true;

	// Build flat detect array: [expanded hail tones (no modulo)] + [suffix]
	for (int s = 0; s < ack_pattern_nsymb; s++)
		hail_detect_tones[s] = hail_tones[s % ack_pattern_len];
	for (int s = 0; s < HAIL_SUFFIX_LEN; s++)
		hail_detect_tones[ack_pattern_nsymb + s] = hail_suffix[s];

	hail_detect_nsymb = ack_pattern_nsymb + HAIL_SUFFIX_LEN;
	hail_detect_threshold = hail_match_threshold + HAIL_SUFFIX_LEN;
}

void cl_mfsk::clear_hail_target()
{
	hail_directed = false;
	for (int i = 0; i < HAIL_SUFFIX_LEN; i++)
		hail_suffix[i] = 0;

	// Undirected: flat array of just the base hail tones
	for (int s = 0; s < ack_pattern_nsymb; s++)
		hail_detect_tones[s] = hail_tones[s % ack_pattern_len];

	hail_detect_nsymb = ack_pattern_nsymb;
	hail_detect_threshold = hail_match_threshold;
}

void cl_mfsk::deinit()
{
	M = 0;
	nBits = 0;
	Nc = 0;
	nStreams = 0;
	tone_hop_step = 0;
	preamble_nSymb = 0;
}

// Generate MFSK preamble: known tones in all streams simultaneously
void cl_mfsk::generate_preamble(std::complex<double>* preamble_out, int nSymb)
{
	if (M == 0 || Nc == 0 || nStreams == 0) return;

	// Amplitude: total power = Nc, split across nStreams tones
	double amp = sqrt((double)Nc / nStreams);

	for (int s = 0; s < nSymb; s++)
	{
		// Zero all subcarriers
		for (int k = 0; k < Nc; k++)
		{
			preamble_out[s * Nc + k] = std::complex<double>(0.0, 0.0);
		}
		// Place known tone in each stream's band
		int tone = preamble_tones[s % preamble_nSymb];
		for (int st = 0; st < nStreams; st++)
		{
			preamble_out[s * Nc + stream_offsets[st] + tone] = std::complex<double>(amp, 0.0);
		}
	}
}

// Generate ACK pattern: ack_pattern_nsymb symbols of known tones.
// WB: 8 base tones with hopping, repeated 2x. NB: full Sidelnikov sequence, no hopping.
void cl_mfsk::generate_ack_pattern(std::complex<double>* pattern_out)
{
	if (M == 0 || Nc == 0 || nStreams == 0) return;

	double amp = sqrt((double)Nc / nStreams);

	for (int s = 0; s < ack_pattern_nsymb; s++)
	{
		for (int k = 0; k < Nc; k++)
		{
			pattern_out[s * Nc + k] = std::complex<double>(0.0, 0.0);
		}

		int tone_base = ack_tones[s % ack_pattern_len];
		int actual_tone = (tone_base + s * tone_hop_step) % M;

		for (int st = 0; st < nStreams; st++)
		{
			pattern_out[s * Nc + stream_offsets[st] + actual_tone] = std::complex<double>(amp, 0.0);
		}
	}
}

// Generate BREAK pattern: identical structure to ACK but with break_tones
void cl_mfsk::generate_break_pattern(std::complex<double>* pattern_out)
{
	if (M == 0 || Nc == 0 || nStreams == 0) return;

	double amp = sqrt((double)Nc / nStreams);

	for (int s = 0; s < ack_pattern_nsymb; s++)
	{
		for (int k = 0; k < Nc; k++)
		{
			pattern_out[s * Nc + k] = std::complex<double>(0.0, 0.0);
		}

		int tone_base = break_tones[s % ack_pattern_len];
		int actual_tone = (tone_base + s * tone_hop_step) % M;

		for (int st = 0; st < nStreams; st++)
		{
			pattern_out[s * Nc + stream_offsets[st] + actual_tone] = std::complex<double>(amp, 0.0);
		}
	}
}

// Generate HAIL pattern: "I am Mercury" prefix + optional CRC suffix for directed hailing.
// When hail_directed is true, appends HAIL_SUFFIX_LEN symbols derived from target callsign.
void cl_mfsk::generate_hail_pattern(std::complex<double>* pattern_out)
{
	if (M == 0 || Nc == 0 || nStreams == 0) return;

	double amp = sqrt((double)Nc / nStreams);

	// Generate all symbols (prefix + suffix) from the flat detect array
	for (int s = 0; s < hail_detect_nsymb; s++)
	{
		for (int k = 0; k < Nc; k++)
		{
			pattern_out[s * Nc + k] = std::complex<double>(0.0, 0.0);
		}

		int tone_base = hail_detect_tones[s];
		int actual_tone = (tone_base + s * tone_hop_step) % M;

		for (int st = 0; st < nStreams; st++)
		{
			pattern_out[s * Nc + stream_offsets[st] + actual_tone] = std::complex<double>(amp, 0.0);
		}
	}
}

// TX: Map groups of bits to one-hot subcarrier vectors across all streams
// Each symbol period consumes nStreams * nBits input bits
void cl_mfsk::mod(const int* bits_in, int total_bits,
                  std::complex<double>* symbols_out)
{
	if (M == 0 || nBits == 0 || Nc == 0 || nStreams == 0) return;

	int bps = nBits * nStreams; // bits per symbol period
	int nSymbols = total_bits / bps;

	// Amplitude: total power = Nc, split across nStreams active tones
	double amp = sqrt((double)Nc / nStreams);

	for (int s = 0; s < nSymbols; s++)
	{
		// Zero all subcarriers for this symbol
		for (int k = 0; k < Nc; k++)
		{
			symbols_out[s * Nc + k] = std::complex<double>(0.0, 0.0);
		}

		// Process each stream
		for (int st = 0; st < nStreams; st++)
		{
			int bit_offset = s * bps + st * nBits;

			// Convert nBits bits to tone index (Gray code mapping)
			int tone_index = 0;
			for (int b = 0; b < nBits; b++)
			{
				if (bits_in[bit_offset + b])
				{
					tone_index |= (1 << (nBits - 1 - b));
				}
			}

			// Gray to binary conversion for better bit-error properties
			int binary_index = tone_index;
			for (int shift = 1; shift < nBits; shift++)
			{
				binary_index ^= (tone_index >> shift);
			}
			tone_index = binary_index;

			if (tone_index >= M) tone_index = M - 1;

			// Apply tone hopping for frequency diversity
			int actual_tone = (tone_index + s * tone_hop_step) % M;

			// Place in this stream's band
			symbols_out[s * Nc + stream_offsets[st] + actual_tone] = std::complex<double>(amp, 0.0);
		}
	}
}

// RX: Non-coherent energy detection across all streams with soft LLR output
void cl_mfsk::demod(const std::complex<double>* fft_in, int total_bits,
                    float* llr_out)
{
	if (M == 0 || nBits == 0 || Nc == 0 || nStreams == 0) return;

	int bps = nBits * nStreams; // bits per symbol period
	int nSymbols = total_bits / bps;

	for (int s = 0; s < nSymbols; s++)
	{
		// Estimate noise variance from bins outside all stream bands
		int total_bins = nStreams * M;
		int band_start = stream_offsets[0];
		int band_end = stream_offsets[nStreams - 1] + M;
		double noise_sum = 0.0;
		int noise_bins = 0;
		for (int k = 0; k < Nc; k++)
		{
			if (k < band_start || k >= band_end)
			{
				std::complex<double> val = fft_in[s * Nc + k];
				double e = val.real() * val.real() + val.imag() * val.imag();
				if (std::isfinite(e)) {
					noise_sum += e;
					noise_bins++;
				}
			}
		}
		double noise_var = (noise_bins > 0) ? noise_sum / noise_bins : 1e-30;
		if (noise_var < 1e-30) noise_var = 1e-30;

		double llr_scale = 1.0 / (2.0 * noise_var);

		// Process each stream independently
		for (int st = 0; st < nStreams; st++)
		{
			// Measure energy in this stream's tone bins
			double E_raw[64]; // M <= 64
			for (int m = 0; m < M; m++)
			{
				std::complex<double> val = fft_in[s * Nc + stream_offsets[st] + m];
				E_raw[m] = val.real() * val.real() + val.imag() * val.imag();
				if (!std::isfinite(E_raw[m])) { E_raw[m] = 0.0; }
			}

			// Reverse tone hopping: E[data_tone] = E_raw[actual_tone]
			double E[64];
			int hop = (s * tone_hop_step) % M;
			for (int m = 0; m < M; m++)
			{
				int actual = (m + hop) % M;
				E[m] = E_raw[actual];
			}

			// Compute LLRs for this stream's bits
			int llr_offset = s * bps + st * nBits;
			for (int k = 0; k < nBits; k++)
			{
				int mask = 1 << (nBits - 1 - k);
				double max_E1 = -1e30;
				double max_E0 = -1e30;

				for (int m = 0; m < M; m++)
				{
					// Convert m to Gray code to match TX mapping
					int gray_m = m ^ (m >> 1);

					if (gray_m & mask)
					{
						if (E[m] > max_E1) max_E1 = E[m];
					}
					else
					{
						if (E[m] > max_E0) max_E0 = E[m];
					}
				}

				double llr = (max_E0 - max_E1) * llr_scale;
				if (!std::isfinite(llr)) llr = 0.0;
				else if (llr > 5.0) llr = 5.0;
				else if (llr < -5.0) llr = -5.0;
				llr_out[llr_offset + k] = (float)llr;
			}
		}
	}
}
