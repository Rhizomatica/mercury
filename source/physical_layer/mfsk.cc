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
	for (int i = 0; i < ACK_PATTERN_LEN; i++)
		ack_tones[i] = 0;
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
	preamble_nSymb = 4;
	if (M == 32)
	{
		preamble_tones[0] = 4;
		preamble_tones[1] = 20;
		preamble_tones[2] = 12;
		preamble_tones[3] = 28;
	}
	else if (M == 16)
	{
		preamble_tones[0] = 2;
		preamble_tones[1] = 10;
		preamble_tones[2] = 6;
		preamble_tones[3] = 14;
	}
	else
	{
		// Generic: spread evenly
		for (int i = 0; i < preamble_nSymb && i < MAX_PREAMBLE_SYMB; i++)
			preamble_tones[i] = (i * M / preamble_nSymb + M / (2 * preamble_nSymb)) % M;
	}

	// ACK pattern tones: Welch-Costas array (p=17, g=5) for optimal autocorrelation.
	// Costas property: all pairwise (dt, df) difference vectors are unique,
	// giving at most 1 coincidence at any non-zero time shift.
	// Values = (5^i mod 17) - 1 for i=1..8.
	if (M == 32)
	{
		// 2x scaled M=16 Costas values. Avoids preamble {4,20,12,28}.
		ack_tones[0] = 8;  ack_tones[1] = 14;
		ack_tones[2] = 10; ack_tones[3] = 24;
		ack_tones[4] = 26; ack_tones[5] = 2;
		ack_tones[6] = 18; ack_tones[7] = 30;
	}
	else if (M == 16)
	{
		// Welch-Costas (p=17, g=5). Avoids preamble {2,6,10,14}.
		ack_tones[0] = 4;  ack_tones[1] = 7;
		ack_tones[2] = 5;  ack_tones[3] = 12;
		ack_tones[4] = 13; ack_tones[5] = 1;
		ack_tones[6] = 9;  ack_tones[7] = 15;
	}
	else
	{
		// Generic: offset from preamble tones
		for (int i = 0; i < ACK_PATTERN_LEN; i++)
			ack_tones[i] = (i * M / ACK_PATTERN_LEN + 1) % M;
	}
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

// Generate ACK pattern: known tones with hopping, repeated ACK_PATTERN_REPS times
void cl_mfsk::generate_ack_pattern(std::complex<double>* pattern_out)
{
	if (M == 0 || Nc == 0 || nStreams == 0) return;

	// Same amplitude as preamble/data: total power = Nc, split across nStreams
	double amp = sqrt((double)Nc / nStreams);

	for (int s = 0; s < ACK_PATTERN_NSYMB; s++)
	{
		// Zero all subcarriers
		for (int k = 0; k < Nc; k++)
		{
			pattern_out[s * Nc + k] = std::complex<double>(0.0, 0.0);
		}

		// Which tone from the ack_tones sequence (wraps with ACK_PATTERN_LEN)
		int tone_base = ack_tones[s % ACK_PATTERN_LEN];

		// Apply tone hopping for frequency diversity (same step as data)
		int actual_tone = (tone_base + s * tone_hop_step) % M;

		// Place in each stream's band (same tone in all streams, like preamble)
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
