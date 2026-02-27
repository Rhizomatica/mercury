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

#include "common/os_interop.h"
#include "physical_layer/ofdm.h"
#include "debug/canary_guard.h"
#include <algorithm>  // for std::swap in optimized FFT

// PocketFFT: high-performance FFT library (BSD license)
// Replaces hand-rolled Cooley-Tukey. ~2-3x faster for N=256.
#define POCKETFFT_NO_MULTITHREADING
#include "physical_layer/pocketfft_hdronly.h"


cl_ofdm::cl_ofdm()
{
	this->Nc=0;
	this->Nfft=0;
	this->Nsymb=0;
	this->gi=0;
	Ngi=0;
	ofdm_frame =NULL;
	ofdm_preamble=NULL;
	zero_padded_data=NULL;
	iffted_data=NULL;
	gi_removed_data=NULL;
	ffted_data=NULL;
	estimated_channel=NULL;
	estimated_channel_without_amplitude_restoration=NULL;
	time_sync_Nsymb=1;
	freq_offset_ignore_limit=0.1;
	start_shift=1;
	passband_start_sample=0;
	preamble_papr_cut=99;
	data_papr_cut=99;
	channel_estimator=ZERO_FORCE;
	LS_window_width=0;
	LS_window_hight=0;
	channel_estimator_amplitude_restoration=NO;
	// Optimized FFT tables
	fft_twiddle=NULL;
	fft_scratch=NULL;
	fft_bit_rev=NULL;
	fft_twiddle_size=0;
	// Pre-allocated passband_to_baseband buffers
	p2b_l_data=NULL;
	p2b_data_filtered=NULL;
	p2b_buffer_size=0;
	// Pre-allocated Nfft work buffers (Group A)
	work_buf_a=NULL;
	work_buf_b=NULL;
	// Pre-allocated time_sync_preamble buffers (Group B)
	tsync_corr_loc=NULL;
	tsync_corr_vals=NULL;
	tsync_corr_size=0;
	tsync_data=NULL;
	tsync_data_size=0;
	// Pre-allocated baseband_to_passband buffer (Group C)
	b2p_data_interpolated=NULL;
	b2p_buffer_size=0;
	// MFSK cross-correlation template (NB only)
	mfsk_corr_template=NULL;
	mfsk_corr_template_len=0;
	mfsk_corr_template_energy=0.0;
	mfsk_corr_template_nsymb=0;
	for(int i=0;i<8;i++) mfsk_corr_template_sym_energy[i]=0.0;
	// OFDM matched-filter template
	ofdm_corr_template=NULL;
	ofdm_corr_template_len=0;
	ofdm_corr_template_nsymb=0;
	ofdm_corr_template_energy=0.0;
	for(int i=0;i<16;i++) ofdm_corr_template_sym_energy[i]=0.0;
}

cl_ofdm::~cl_ofdm()
{
	this->deinit();
}

// Forward declaration — defined after butterfly variants below
void init_simd_dispatch();

void cl_ofdm::init(int Nfft, int Nc, int Nsymb, float gi)
{
	this->Nc=Nc;
	this->Nfft=Nfft;
	this->Nsymb=Nsymb;
	this->gi=gi;
	if(LS_window_width==0)
	{
		LS_window_width=Nc;
	}
	if(LS_window_hight==0)
	{
		LS_window_hight=Nsymb;
	}

	this->init();
}
void cl_ofdm::init()
{
	Ngi=Nfft*gi;

	ofdm_frame = CNEW(struct st_carrier, this->Nsymb*this->Nc, "ofdm.ofdm_frame");
	zero_padded_data=CNEW(std::complex<double>, Nfft, "ofdm.zero_padded_data");
	iffted_data=CNEW(std::complex<double>, Nfft, "ofdm.iffted_data");
	gi_removed_data=CNEW(std::complex<double>, Nfft, "ofdm.gi_removed_data");
	ffted_data=CNEW(std::complex<double>, Nfft, "ofdm.ffted_data");
	estimated_channel=CNEW(struct st_channel_complex, this->Nsymb*this->Nc, "ofdm.estimated_channel");
	estimated_channel_without_amplitude_restoration=CNEW(struct st_channel_complex, this->Nsymb*this->Nc, "ofdm.est_channel_noamp");
	ofdm_preamble = CNEW(struct st_carrier, this->preamble_configurator.Nsymb*this->Nc, "ofdm.ofdm_preamble");
	passband_start_sample=0;

	preamble_configurator.init(this->Nfft, this->Nc,this->ofdm_preamble, this->start_shift);
	pilot_configurator.init(this->Nfft, this->Nc,this->Nsymb,this->ofdm_frame, this->start_shift);

	// Initialize optimized FFT tables + SIMD dispatch
	init_fft_tables(this->Nfft);
	static bool simd_initialized = false;
	if (!simd_initialized) {
		init_simd_dispatch();
		simd_initialized = true;
	}

	// Pre-allocate shared Nfft work buffers (used by frequency_sync_coarse,
	// time_sync_mfsk, detect_ack_pattern — never called concurrently)
	work_buf_a = CNEW(std::complex<double>, Nfft, "ofdm.work_buf_a");
	work_buf_b = CNEW(std::complex<double>, Nfft, "ofdm.work_buf_b");

	for(int i=0;i<Nsymb;i++)
	{
		for(int j=0;j<Nc;j++)
		{
			(estimated_channel+i*Nc+j)->value=1;
		}
	}
}

void cl_ofdm::deinit()
{
	this->Ngi=0;
	this->Nc=0;
	this->Nfft=0;
	this->Nsymb=0;
	this->gi=0;

	pilot_configurator.Dx=0;
	pilot_configurator.Dy=0;
	pilot_configurator.first_row=0;
	pilot_configurator.last_row=0;
	pilot_configurator.first_col=0;
	pilot_configurator.second_col=0;
	pilot_configurator.last_col=0;
	pilot_configurator.boost=0;

	preamble_configurator.Nsymb=0;
	preamble_configurator.nIdentical_sections=0;
	preamble_configurator.modulation=0;
	preamble_configurator.boost=0;


	CDELETE(ofdm_frame);
	CDELETE(ofdm_preamble);
	CDELETE(zero_padded_data);
	CDELETE(iffted_data);
	CDELETE(gi_removed_data);
	CDELETE(ffted_data);
	CDELETE(estimated_channel);
	CDELETE(estimated_channel_without_amplitude_restoration);
	if(p2b_l_data!=NULL){delete[] p2b_l_data; p2b_l_data=NULL;}
	if(p2b_data_filtered!=NULL){delete[] p2b_data_filtered; p2b_data_filtered=NULL;}
	p2b_buffer_size=0;
	CDELETE(work_buf_a);
	CDELETE(work_buf_b);
	if(tsync_corr_loc!=NULL){delete[] tsync_corr_loc; tsync_corr_loc=NULL;}
	if(tsync_corr_vals!=NULL){delete[] tsync_corr_vals; tsync_corr_vals=NULL;}
	tsync_corr_size=0;
	if(tsync_data!=NULL){delete[] tsync_data; tsync_data=NULL;}
	tsync_data_size=0;
	if(b2p_data_interpolated!=NULL){delete[] b2p_data_interpolated; b2p_data_interpolated=NULL;}
	b2p_buffer_size=0;
	CDELETE(mfsk_corr_template);
	mfsk_corr_template_len=0;
	mfsk_corr_template_energy=0.0;
	mfsk_corr_template_nsymb=0;
	for(int i=0;i<8;i++) mfsk_corr_template_sym_energy[i]=0.0;
	CDELETE(ofdm_corr_template);
	ofdm_corr_template_len=0;
	ofdm_corr_template_nsymb=0;
	ofdm_corr_template_energy=0.0;
	for(int i=0;i<16;i++) ofdm_corr_template_sym_energy[i]=0.0;

	pilot_configurator.deinit();
	preamble_configurator.deinit();
	deinit_fft_tables();
}

// ============================================================================
// ============================================================================
// PocketFFT-based FFT (replaces hand-rolled SIMD-dispatched Cooley-Tukey)
// PocketFFT uses mixed-radix decomposition + auto-vectorization.
// ============================================================================

void init_simd_dispatch()
{
	printf("[FFT] Using PocketFFT (mixed-radix, auto-vectorized)\n");
	fflush(stdout);
}

void cl_ofdm::init_fft_tables(int n)
{
	if (n <= 0 || (n & (n-1)) != 0) {
		// n must be power of 2
		fft_twiddle_size = 0;
		return;
	}

	fft_twiddle_size = n;

	// Allocate twiddle factors (only need n/2 for radix-2)
	fft_twiddle = CNEW(std::complex<double>, n/2, "ofdm.fft_twiddle");
	for (int k = 0; k < n/2; k++) {
		double angle = -2.0 * M_PI * k / n;
		fft_twiddle[k] = std::complex<double>(cos(angle), sin(angle));
	}

	// Allocate scratch buffer
	fft_scratch = CNEW(std::complex<double>, n, "ofdm.fft_scratch");

	// Build bit-reversal permutation table
	fft_bit_rev = CNEW(int, n, "ofdm.fft_bit_rev");
	int bits = 0;
	for (int temp = n; temp > 1; temp >>= 1) bits++;

	for (int i = 0; i < n; i++) {
		int rev = 0;
		for (int j = 0; j < bits; j++) {
			if (i & (1 << j)) {
				rev |= (1 << (bits - 1 - j));
			}
		}
		fft_bit_rev[i] = rev;
	}
}

void cl_ofdm::deinit_fft_tables()
{
	CDELETE(fft_twiddle);
	CDELETE(fft_scratch);
	CDELETE(fft_bit_rev);
	fft_twiddle_size = 0;
}

// Optimized FFT — PocketFFT (mixed-radix, auto-vectorized)
void cl_ofdm::_fft_fast(std::complex<double>* v, int n)
{
	pocketfft::shape_t shape{(size_t)n};
	pocketfft::stride_t stride{(ptrdiff_t)sizeof(std::complex<double>)};
	pocketfft::shape_t axes{0};
	pocketfft::c2c(shape, stride, stride, axes, pocketfft::FORWARD, v, v, 1.0);
}

// Optimized IFFT — PocketFFT (mixed-radix, auto-vectorized)
void cl_ofdm::_ifft_fast(std::complex<double>* v, int n)
{
	pocketfft::shape_t shape{(size_t)n};
	pocketfft::stride_t stride{(ptrdiff_t)sizeof(std::complex<double>)};
	pocketfft::shape_t axes{0};
	pocketfft::c2c(shape, stride, stride, axes, pocketfft::BACKWARD, v, v, 1.0);
}

void cl_ofdm::zero_padder(std::complex <double>* in, std::complex <double>* out)
{
	for(int j=0;j<Nc/2;j++)
	{
		out[j+Nfft-Nc/2]=in[j];
	}

	for(int j=0;j<start_shift;j++)
	{
		out[j]=std::complex <double>(0,0);
	}

	for(int j=Nc/2+start_shift;j<Nfft-Nc/2;j++)
	{
		out[j]=std::complex <double>(0,0);
	}

	for(int j=Nc/2;j<Nc;j++)
	{
		out[j-Nc/2+start_shift]=in[j];
	}
}
void cl_ofdm::zero_depadder(std::complex <double>* in, std::complex <double>* out)
{
	for(int j=0;j<Nc/2;j++)
	{
		out[j]=in[j+Nfft-Nc/2];
	}
	for(int j=Nc/2;j<Nc;j++)
	{
		out[j]=in[j-Nc/2+start_shift];
	}
}
void cl_ofdm::gi_adder(std::complex <double>* in, std::complex <double>* out)
{
	for(int j=0;j<Nfft;j++)
	{
		out[j+Ngi]=in[j];
	}
	for(int j=0;j<Ngi;j++)
	{
		out[j]=in[j+Nfft-Ngi];
	}
}
void cl_ofdm::gi_remover(std::complex <double>* in, std::complex <double>* out)
{
	for(int j=0;j<Nfft;j++)
	{
		out[j]=in[j+Ngi];
	}
}

void cl_ofdm::fft(std::complex <double>* in, std::complex <double>* out)
{
	for(int i=0;i<Nfft;i++)
	{
		out[i]=in[i];
	}
	_fft_fast(out,Nfft);  // Use optimized FFT

	for(int i=0;i<Nfft;i++)
	{
		out[i]=out[i]/(double)Nfft;
	}

}
void cl_ofdm::fft(std::complex <double>* in, std::complex <double>* out, int _Nfft)
{
	for(int i=0;i<_Nfft;i++)
	{
		out[i]=in[i];
	}
	_fft_fast(out,_Nfft);  // Use optimized FFT

	for(int i=0;i<_Nfft;i++)
	{
		out[i]=out[i]/(double)_Nfft;
	}

}

void cl_ofdm::_fft(std::complex <double> *v, int n)
{
	if(n>1) {
		std::complex <double> *tmp=new std::complex <double>[n];
		int k,m;    std::complex <double> z, w, *vo, *ve;
		ve = tmp; vo = tmp+n/2;
		for(k=0; k<n/2; k++) {
			ve[k] = v[2*k];
			vo[k] = v[2*k+1];
		}
		_fft( ve, n/2 );
		_fft( vo, n/2 );
		for(m=0; m<n/2; m++) {
			w.real( cos(2*M_PI*m/(double)n));
			w.imag( -sin(2*M_PI*m/(double)n));
			z.real( w.real()*vo[m].real() - w.imag()*vo[m].imag());
			z.imag( w.real()*vo[m].imag() + w.imag()*vo[m].real());
			v[  m  ].real( ve[m].real() + z.real());
			v[  m  ].imag( ve[m].imag() + z.imag());
			v[m+n/2].real( ve[m].real() - z.real());
			v[m+n/2].imag( ve[m].imag() - z.imag());
		}
		if(tmp!=NULL)
		{
			delete[] tmp;
		}
	}
	//Ref:Wickerhauser, Mladen Victor,Mathematics for Multimedia, Birkhäuser Boston, January 2010, DOI: 10.1007/978-0-8176-4880-0, ISBNs 978-0-8176-4880-0, 978-0-8176-4879-4
	//https://www.math.wustl.edu/~victor/mfmm/
}

void cl_ofdm::ifft(std::complex <double>* in, std::complex <double>* out)
{
	for(int i=0;i<Nfft;i++)
	{
		out[i]=in[i];
	}
	_ifft_fast(out,Nfft);  // Use optimized IFFT
}

void cl_ofdm::ifft(std::complex <double>* in, std::complex <double>* out,int _Nfft)
{
	for(int i=0;i<_Nfft;i++)
	{
		out[i]=in[i];
	}
	_ifft_fast(out,_Nfft);  // Use optimized IFFT
}

void cl_ofdm::_ifft(std::complex <double>* v,int n)
{
	if(n>1) {
		std::complex <double> *tmp=new std::complex <double>[n];
		int k,m;    std::complex <double> z, w, *vo, *ve;
		ve = tmp; vo = tmp+n/2;
		for(k=0; k<n/2; k++) {
			ve[k] = v[2*k];
			vo[k] = v[2*k+1];
		}
		_ifft( ve, n/2);
		_ifft( vo, n/2);
		for(m=0; m<n/2; m++) {
			w.real( cos(2*M_PI*m/(double)n));
			w.imag( sin(2*M_PI*m/(double)n));
			z.real( w.real()*vo[m].real() - w.imag()*vo[m].imag());
			z.imag( w.real()*vo[m].imag() + w.imag()*vo[m].real());
			v[  m  ].real( ve[m].real() + z.real());
			v[  m  ].imag( ve[m].imag() + z.imag());
			v[m+n/2].real( ve[m].real() - z.real());
			v[m+n/2].imag( ve[m].imag() - z.imag());
		}
		if(tmp!=NULL)
		{
			delete[] tmp;
		}
	}
	//Ref:Wickerhauser, Mladen Victor,Mathematics for Multimedia, Birkhäuser Boston, January 2010, DOI: 10.1007/978-0-8176-4880-0, ISBNs 978-0-8176-4880-0, 978-0-8176-4879-4
	//https://www.math.wustl.edu/~victor/mfmm/
}

double cl_ofdm::carrier_sampling_frequency_sync(std::complex <double>*in, double carrier_freq_width, int preamble_nSymb, double sampling_frequency)
{
	double frequency_offset_prec=0;

	std::complex <double> p1,p2,mul;
	std::complex <double> frame[Nfft];
	std::complex <double> frame_fft[Nfft],frame_depadded1[Nfft],frame_depadded2[Nfft];

	if(preamble_nSymb/2==0)
	{
		preamble_nSymb=1;
	}
	else
	{
		preamble_nSymb/=2;
	}

	mul=0;
	for(int j=0;j<preamble_nSymb;j++)
	{
		for(int i=0;i<Nfft/2;i++)
		{
			frame[i]=*(in+j*(Nfft+Ngi)+i);
			frame[i+Nfft/2]=*(in+j*(Nfft+Ngi)+i);
		}

		fft(frame,frame_fft);
		zero_depadder(frame_fft,frame_depadded1);

		for(int i=0;i<Nfft/2;i++)
		{
			frame[i]=*(in+j*(Nfft+Ngi)+i+Nfft/2);
			frame[i+Nfft/2]=*(in+j*(Nfft+Ngi)+i+Nfft/2);
		}

		fft(frame,frame_fft);
		zero_depadder(frame_fft,frame_depadded2);


		for(int i=0;i<Nc;i++)
		{
			mul+=conj(frame_depadded2[i])*frame_depadded1[i];
		}
	}


	frequency_offset_prec = get_angle(mul) / M_PI;

	// float sampling_frequency_offset= -frequency_offset_prec*carrier_freq_width /sampling_frequency;

	return (frequency_offset_prec*carrier_freq_width);

	//Ref1: P. H. Moose, "A technique for orthogonal frequency division multiplexing frequency offset correction," in IEEE Transactions on Communications, vol. 42, no. 10, pp. 2908-2914, Oct. 1994, doi: 10.1109/26.328961.
	//Ref2: T. M. Schmidl and D. C. Cox, "Robust frequency and timing synchronization for OFDM," in IEEE Transactions on Communications, vol. 45, no. 12, pp. 1613-1621, Dec. 1997, doi: 10.1109/26.650240.
	//Ref3: M. Speth, S. Fechtel, G. Fock and H. Meyr, "Optimum receiver design for OFDM-based broadband transmission .II. A case study," in IEEE Transactions on Communications, vol. 49, no. 4, pp. 571-578, April 2001, doi: 10.1109/26.917759.
}

double cl_ofdm::carrier_frequency_sync_nb(std::complex<double>* in, double carrier_freq_width, int preamble_nSymb)
{
	/*
	 * Cross-symbol phase progression frequency estimator for narrowband OFDM.
	 *
	 * NB preamble uses ALL subcarriers (Nc=10), which breaks Moose's
	 * half-symbol repetition assumption (requires even-only bins).
	 *
	 * Instead, measure phase rotation between adjacent preamble symbols:
	 * 1. FFT each preamble symbol, remove known modulation → channel estimate
	 * 2. Cross-correlate adjacent symbols: C = Σ H[sym+1] × conj(H[sym])
	 * 3. Phase of C = 2π × freq_offset × T_symbol
	 *
	 * Capture range: ±fs/(2×Nofdm) = ±22 Hz for NB (Nfft=256, Ngi=16).
	 * 10 subcarriers × (nSymb-1) pairs gives robust averaging.
	 *
	 * Input convention matches Moose: in = &baseband_data[Ngi]
	 * Internal access: in[sym * (Nfft+Ngi) + k] for k=0..Nfft-1
	 */

	int Nofdm = Nfft + Ngi;
	std::complex<double> fft_in[256];
	std::complex<double> fft_out[256];
	std::complex<double> depadded[256];
	std::complex<double> H_prev[256];
	std::complex<double> H_cur[256];

	std::complex<double> C(0.0, 0.0);
	double energy_total = 0.0;

	for (int sym = 0; sym < preamble_nSymb; sym++)
	{
		// FFT this preamble symbol
		for (int k = 0; k < Nfft; k++)
			fft_in[k] = in[sym * Nofdm + k];

		fft(fft_in, fft_out, Nfft);
		zero_depadder(fft_out, depadded);

		// Remove known modulation → raw channel estimate
		for (int k = 0; k < Nc; k++)
		{
			if (ofdm_preamble[sym * Nc + k].type == PREAMBLE)
				H_cur[k] = depadded[k] * std::conj(ofdm_preamble[sym * Nc + k].value);
			else
				H_cur[k] = std::complex<double>(0.0, 0.0);

			energy_total += std::norm(H_cur[k]);
		}

		// Cross-symbol correlation (sym >= 1)
		if (sym > 0)
		{
			for (int k = 0; k < Nc; k++)
				C += H_cur[k] * std::conj(H_prev[k]);
		}

		for (int k = 0; k < Nc; k++)
			H_prev[k] = H_cur[k];
	}

	// Confidence gate: reject if correlation is weak relative to energy
	double C_mag = std::abs(C);
	if (energy_total < 1e-10 || C_mag / energy_total < 0.05)
		return 0.0;

	// passband_to_baseband uses exp(+j×2πfc×t), so a carrier offset δ Hz
	// produces baseband phase exp(-j×2πδ×t). Cross-symbol phase is -2πδ×T.
	// Therefore: δ = -arg(C) / (2π × T_symbol)
	// T_symbol = Nofdm / fs_base, fs_base = carrier_freq_width × Nfft
	double phase = std::arg(C);
	double freq_offset = -phase * carrier_freq_width * (double)Nfft / (2.0 * M_PI * (double)Nofdm);

	return freq_offset;
}

double cl_ofdm::frequency_sync_coarse(std::complex<double>* in, double subcarrier_spacing, int search_range_subcarriers, int interpolation_rate)
{
	/*
	 * Full Schmidl-Cox frequency synchronization with integer CFO estimation.
	 *
	 * The preamble uses alternating carriers (even bins only), creating
	 * time-domain repetition with period Nfft/2.
	 *
	 * Fractional CFO (within ±0.5 subcarrier spacing):
	 *   - Correlate first half with second half of OFDM symbol
	 *   - Phase of correlation = 2π × ε_frac × (Nfft/2) / Nfft = π × ε_frac
	 *   - So: ε_frac = angle(P) / π (in subcarrier spacings)
	 *
	 * Integer CFO (multiples of subcarrier spacing):
	 *   - After fractional correction, FFT the preamble
	 *   - Correlate received spectrum with known preamble pattern at different shifts
	 *   - Peak correlation indicates integer offset
	 *
	 * Input: baseband signal at interpolation_rate (e.g., 4x for 48kHz/12kHz)
	 * Output: total frequency offset in Hz
	 *
	 * Ref: T. M. Schmidl and D. C. Cox, "Robust frequency and timing
	 *      synchronization for OFDM," IEEE Trans. Comm., vol. 45, no. 12,
	 *      pp. 1613-1621, Dec. 1997.
	 */

	// Step 1: Fractional CFO estimation from time-domain correlation
	// Correlate first half with second half of each preamble symbol
	std::complex<double> P(0.0, 0.0);  // Complex correlation
	double R = 0.0;  // Normalization (energy of second half)

	// Account for interpolation rate in sample indices
	int half_symbol = (Nfft * interpolation_rate) / 2;
	int gi_samples = Ngi * interpolation_rate;

	// Use first preamble symbol (after GI)
	std::complex<double>* symbol_start = in + gi_samples;

	// Step 0: Energy gate - check signal level before CFO estimation
	// This prevents noise from producing bogus CFO estimates
	double input_energy = 0.0;
	for (int n = 0; n < Nfft; n++) {
		int src_idx = n * interpolation_rate;
		input_energy += std::norm(symbol_start[src_idx]);
	}

	// Energy threshold - tune this based on observed signal vs noise levels
	// Typical signal energy is ~10-100, noise is <1
	const double min_energy = 1.0;
	if (input_energy < min_energy) {
		return 0.0;  // No signal, don't apply any correction
	}

	for (int n = 0; n < half_symbol; n++)
	{
		std::complex<double> first_half = symbol_start[n];
		std::complex<double> second_half = symbol_start[n + half_symbol];

		// P = Σ r(n) × r*(n + Nfft/2)
		P += first_half * std::conj(second_half);
		R += std::norm(second_half);
	}

	// Fractional CFO in subcarrier spacings
	// For frequency offset ε (in subcarrier spacings):
	// r(n+N/2) = r(n) × exp(jπε), so P = Σ|r(n)|² × exp(-jπε)
	// Therefore: ε = -arg(P) / π
	double frac_cfo_subcarriers = -std::arg(P) / M_PI;

	// Correlation quality check
	double corr_mag = (R > 0.0) ? (std::abs(P) / R) : 0.0;

	// Gate on correlation quality - low correlation means we're not looking at a valid preamble
	// A good preamble detection should have corr_mag > 0.5
	const double min_corr_mag = 0.5;
	if (corr_mag < min_corr_mag) {
		return 0.0;
	}

	// Note: We don't early exit here because the fractional CFO from noise
	// is typically close to 0 anyway, and we'll check confidence on the
	// integer CFO estimate before applying any large corrections.

	// Step 2: Integer CFO estimation
	// Apply fractional correction and FFT the preamble symbol
	// Note: Input is at interpolation_rate, so we downsample by taking every
	// interpolation_rate-th sample to get back to baseband (Nfft samples)
	std::complex<double>* corrected_symbol = work_buf_a;
	std::complex<double>* fft_out = work_buf_b;

	// Apply fractional frequency correction and downsample to baseband rate
	// Each output sample n corresponds to input sample n*interpolation_rate
	double phase_inc = -2.0 * M_PI * frac_cfo_subcarriers / Nfft;
	for (int n = 0; n < Nfft; n++)
	{
		int src_idx = n * interpolation_rate;
		double phase = phase_inc * n;
		std::complex<double> correction(std::cos(phase), std::sin(phase));
		corrected_symbol[n] = symbol_start[src_idx] * correction;
	}

	// FFT the corrected preamble symbol
	fft(corrected_symbol, fft_out);

	// Note: Correlation quality check disabled - fractional estimate is used directly
	// The filter in telecom_system.cc handles bogus values

	// Integer CFO search is disabled when search_range_subcarriers <= 0
	// (The integer CFO detection via even/odd pattern matching isn't working reliably)
	int best_int_cfo = 0;
	double best_metric = 0.0;
	int search_limit = search_range_subcarriers;

	// Integer CFO search (only if enabled)
	if (search_limit > 0)
	{
		// Limit to valid range
		if (search_limit > Nc / 2) search_limit = Nc / 2;

		for (int k = -search_limit; k <= search_limit; k++)
		{
			double energy_data = 0.0;
			double energy_null = 0.0;

			// Check only active carrier bins (not all Nfft bins)
			for (int carrier = 0; carrier < Nc; carrier++)
			{
				// Map carrier index to FFT bin (same mapping as zero_depadder)
				int fft_bin;
				if (carrier < Nc / 2)
				{
					fft_bin = Nfft - Nc / 2 + carrier;  // Negative frequencies
				}
				else
				{
					fft_bin = carrier - Nc / 2 + start_shift;  // Positive frequencies
				}

				// Apply trial offset to see where this carrier's energy would be
				int received_bin = fft_bin + k;
				if (received_bin < 0) received_bin += Nfft;
				if (received_bin >= Nfft) received_bin -= Nfft;

				// The ORIGINAL FFT bin (before any offset) determines even/odd
				// Even original bins had data, odd original bins were null
				bool should_have_data = ((fft_bin % 2) == 0);

				double bin_energy = std::norm(fft_out[received_bin]);

				if (should_have_data)
				{
					energy_data += bin_energy;
				}
				else
				{
					energy_null += bin_energy;
				}
			}

			// Metric: ratio of energy on expected-data bins vs expected-null bins
			double metric = (energy_null > 0.001) ? (energy_data / energy_null) : energy_data;

			if (metric > best_metric)
			{
				best_metric = metric;
				best_int_cfo = k;
			}
		}
	}

	// Determine effective integer CFO (only use if confident)
	int effective_int_cfo = 0;
	if (search_limit > 0 && best_metric > 2.0)
	{
		effective_int_cfo = best_int_cfo;
	}

	// Total CFO: fractional + integer (if confident)
	double total_cfo_subcarriers = frac_cfo_subcarriers + (double)effective_int_cfo;
	double total_cfo_hz = total_cfo_subcarriers * subcarrier_spacing;

	return total_cfo_hz;
}

void cl_ofdm::framer(std::complex <double>* in, std::complex <double>* out)
{
	int data_index=0;
	int pilot_index=0;
	for(int j=0;j<Nsymb;j++)
	{
		for(int k=0;k<Nc;k++)
		{
			if((ofdm_frame+j*this->Nc+k)->type==DATA)
			{
				out[j*Nc+k]=in[data_index];
				data_index++;
			}
			else if ((ofdm_frame+j*this->Nc+k)->type==PILOT)
			{
				out[j*Nc+k]=pilot_configurator.sequence[pilot_index];
				pilot_index++;
			}
		}
	}

}

void cl_ofdm::deframer(std::complex <double>* in, std::complex <double>* out)
{
	int data_index=0;

	for(int j=0;j<Nsymb;j++)
	{
		for(int k=0;k<Nc;k++)
		{
			if((ofdm_frame+j*this->Nc+k)->type==DATA)
			{
				out[data_index]=in[j*Nc+k];
				data_index++;
			}
		}
	}
}


void cl_ofdm::symbol_mod(std::complex <double>*in, std::complex <double>*out)
{
	zero_padder(in,zero_padded_data);
	ifft(zero_padded_data,iffted_data);
	gi_adder(iffted_data, out);
}

void cl_ofdm::symbol_demod(std::complex <double>*in, std::complex <double>*out)
{
	gi_remover(in, gi_removed_data);
	fft(gi_removed_data,ffted_data);
	zero_depadder(ffted_data, out);
}

cl_pilot_configurator::cl_pilot_configurator()
{
	first_col=DATA;
	last_col=AUTO_SELLECT;
	second_col=DATA;
	first_row=DATA;
	last_row=DATA;
	Nc=0;
	Nsymb=0;
	Nc_max=0;
	nData=0;
	nPilots=0;
	nConfig=0;
	carrier=0;
	Dy=0;
	Dx=0;
	virtual_carrier=0;
	modulation=DBPSK;
	sequence=0;
	boost=1.0;
	Nfft=0;
	start_shift=0;
	seed=0;
	print_on=NO;
	pilot_density=HIGH_DENSITY;
}

cl_pilot_configurator::~cl_pilot_configurator()
{
	CDELETE(virtual_carrier);
}

void cl_pilot_configurator::init(int Nfft, int Nc, int Nsymb,struct st_carrier* _carrier, int start_shift)
{
	this->carrier=_carrier;
	this->Nc=Nc;
	this->Nsymb=Nsymb;
	this->Nfft=Nfft;
	this->start_shift=start_shift;
	if(Nc>Nsymb)
	{
		this->Nc_max=Nc;
	}
	else
	{
		this->Nc_max=Nsymb;
	}
	nData=Nc*Nsymb;
	virtual_carrier = CNEW(struct st_carrier, this->Nc_max*this->Nc_max, "pilot.virtual_carrier");

	for(int j=0;j<this->Nc_max;j++)
	{
		for(int i=0;i<this->Nc_max;i++)
		{
			(virtual_carrier+j*this->Nc_max+i)->type=DATA;
		}

	}

	this->configure();

	sequence = CNEW(std::complex<double>, nPilots, "pilot.sequence");

	if(print_on==YES)
	{
		this->print();
	}

	__srandom(seed);
	int last_pilot=0;
	int pilot_value;
	if(this->modulation==DBPSK)
	{
		for(int i=0;i<nPilots;i++)
		{
			pilot_value=__random()%2 ^ last_pilot;
			sequence[i]=std::complex <double>(2*pilot_value-1,0)*boost;
			last_pilot=pilot_value;
		}
	}
}

void cl_pilot_configurator::deinit()
{
	this->carrier=NULL;
	this->Nc=0;
	this->Nsymb=0;
	this->Nfft=0;
	this->Nc_max=0;
	this->nData=0;

	CDELETE(virtual_carrier);
	CDELETE(sequence);

}

void cl_pilot_configurator::configure()
{
	int x=0;
	int y=0;

	while(x<Nc_max && y<Nc_max)
	{
		(virtual_carrier+y*Nc_max+x)->type=PILOT;

		for(int j=y;j<Nc_max;j+=Dy)
		{
			(virtual_carrier+j*Nc_max+x)->type=PILOT;
		}
		for(int j=y;j>=0;j-=Dy)
		{
			(virtual_carrier+j*Nc_max+x)->type=PILOT;
		}

		y++;
		x+=Dx;

	}

	int pilot_count=0;
	for(int j=0;j<Nsymb;j++)
	{
		if ((virtual_carrier+j*Nc_max+Nc-1)->type==PILOT)
		{
			pilot_count++;
		}
	}

	if(last_col==AUTO_SELLECT && pilot_count<2)
	{
		last_col=COPY_FIRST_COL;
	}


	for(int j=0;j<Nc_max;j++)
	{
		if(first_row==PILOT)
		{
			(virtual_carrier+0*Nc_max+j)->type=PILOT;
		}
		if(last_row==PILOT)
		{
			(virtual_carrier+(Nsymb-1)*Nc_max+j)->type=PILOT;
		}
		if(first_col==PILOT)
		{
			(virtual_carrier+j*Nc_max+0)->type=PILOT;
		}
		if(last_col==PILOT)
		{
			(virtual_carrier+j*Nc_max+Nc-1)->type=PILOT;
		}
		if(last_col==COPY_FIRST_COL)
		{
			(virtual_carrier+j*Nc_max+Nc-1)->type=(virtual_carrier+j*Nc_max+0)->type;
		}
		if(second_col==CONFIG&&(virtual_carrier+j*Nc_max+1)->type!=PILOT)
		{
			(virtual_carrier+j*Nc_max+1)->type=CONFIG;
		}
	}


	nPilots=0;
	nConfig=0;
	for(int j=0;j<Nsymb;j++)
	{
		for(int i=0;i<Nc;i++)
		{

			(carrier + j*Nc+i)->type=(virtual_carrier+j*Nc_max+i)->type;

			if((virtual_carrier+j*Nc_max+i)->type==PILOT)
			{
				nPilots++;
				nData--;
			}
			if((virtual_carrier+j*Nc_max+i)->type==CONFIG)
			{
				nConfig++;
				nData--;
			}
		}
	}
}

void cl_pilot_configurator::print()
{
	for(int j=0;j<Nsymb;j++)
	{
		for(int i=0;i<Nc;i++)
		{
			if((carrier+j*Nc+i)->type==PILOT)
			{
				std::cout<<"P ";
			}
			else if((carrier+j*Nc+i)->type==CONFIG)
			{
				std::cout<<"C ";
			}
			else if((carrier+j*Nc+i)->type==ZERO)
			{
				std::cout<<"Z ";
			}
			else if((carrier+j*Nc+i)->type==PREAMBLE)
			{
				std::cout<<"R ";
			}
			else if((carrier+j*Nc+i)->type==DATA)
			{
				std::cout<<". ";
			}
			else
			{
				std::cout<<"_ ";
			}
		}
		std::cout<<std::endl;

	}

	std::cout<<"nData="<<this->nData<<std::endl;
	std::cout<<"nPilots="<<this->nPilots<<std::endl;
	std::cout<<"nConfig="<<this->nConfig<<std::endl;
}


cl_preamble_configurator::cl_preamble_configurator()
{
	Nc=0;
	Nsymb=0;
	nPreamble=0;
	carrier=0;
	modulation=MOD_BPSK;
	nIdentical_sections=0;
	sequence=0;
	boost=1.0;
	Nfft=0;
	nZeros=0;
	start_shift=0;
	seed=0;
	print_on=NO;
}

cl_preamble_configurator::~cl_preamble_configurator()
{
}

void cl_preamble_configurator::init(int Nfft, int Nc, struct st_carrier* _carrier, int start_shift)
{
	this->carrier=_carrier;
	this->Nc=Nc;
	this->Nfft=Nfft;
	this->start_shift=start_shift;

	this->configure();

	sequence = CNEW(std::complex<double>, this->Nsymb*this->Nc, "preamble.sequence");

	if(print_on==YES)
	{
		this->print();
	}

	// Zadoff-Chu preamble values. ZC sequences have zero periodic
	// auto-correlation sidelobes → sharper matched-filter peak, better
	// discrimination at low SNR. Same sequence repeated each symbol.
	int bins_per_sym = nPreamble / this->Nsymb;
	int zc_root = 7;  // coprime with bins_per_sym for WB (25) and NB (4-5)
	int seq_idx = 0;
	for(int j=0;j<this->Nsymb;j++)
	{
		int sym_preamble_idx = 0;
		for(int i=0;i<this->Nc;i++)
		{
			if ((carrier+j*this->Nc+i)->type==ZERO)
			{
				(carrier+j*this->Nc+i)->value=0;
			}
			else if ((carrier+j*this->Nc+i)->type==PREAMBLE)
			{
				double phase = -M_PI * zc_root * sym_preamble_idx * (sym_preamble_idx + 1.0) / bins_per_sym;
				std::complex<double> zc_val(cos(phase), sin(phase));
				(carrier+j*this->Nc+i)->value = zc_val;
				sequence[seq_idx] = zc_val;
				sym_preamble_idx++;
				seq_idx++;
			}
		}
	}

}

void cl_preamble_configurator::deinit()
{
	this->carrier=NULL;
	this->Nc=0;
	this->Nsymb=0;
	this->Nfft=0;

	CDELETE(sequence);

}

void cl_preamble_configurator::configure()
{

	int fft_zeros_tmp[Nfft];
	int fft_zeros_depadded_tmp[Nc];

	// Even-only subcarrier preamble: creates half-symbol periodicity (L=Nfft/2)
	// for Schmidl-Cox detection, which works at any timing offset.
	// Previously NB (Nc<=10) used all subcarriers for FFT-based detection,
	// but that required GI-aligned search grid (only 5.9% hit rate — Bug #41).
	// With even-only, NB gets 4 preamble subcarriers (bins 252,254,2,4).
	// WB gets ~24 (Nc/2). Both use time_sync_preamble_halfsym().
	bool all_preamble = false;

	for(int j=0;j<Nfft;j++)
	{
		if(all_preamble)
		{
			fft_zeros_tmp[j]=1;
		}
		else if(j%2==1)
		{
			fft_zeros_tmp[j]=0;
		}
		else
		{
			fft_zeros_tmp[j]=1;
		}
	}

	for(int j=0;j<Nc/2;j++)
	{
		fft_zeros_depadded_tmp[j]=fft_zeros_tmp[j+Nfft-Nc/2];
	}
	for(int j=Nc/2;j<Nc;j++)
	{
		fft_zeros_depadded_tmp[j]=fft_zeros_tmp[j-Nc/2+start_shift];
	}

	for(int i=0;i<this->Nsymb;i++)
	{
		for(int j=0;j<Nc;j++)
		{
			if(fft_zeros_depadded_tmp[j]==0)
			{
				(carrier+i*Nc+j)->type=ZERO;
				nZeros++;
			}
			else
			{
				(carrier+i*Nc+j)->type=PREAMBLE;
				nPreamble++;
			}
		}
	}

	// Note: nZeros and nPreamble are already accumulated correctly in the nested loop above.
	// The *= Nsymb lines were removed as they caused double-counting (values were Nsymb^2 too large).

}

void cl_preamble_configurator::print()
{
    std::cout<<"nZeros="<<this->nZeros<<std::endl;
    std::cout<<"nPreamble="<<this->nPreamble<<std::endl;

	for(int j=0;j<Nsymb;j++)
	{
		for(int i=0;i<Nc;i++)
		{
			if((carrier+j*Nc+i)->type==ZERO)
			{
				std::cout<<"Z ";
			}
			else if((carrier+j*Nc+i)->type==PREAMBLE)
			{
				std::cout<<"R ";
			}
			else
			{
				std::cout<<"_ ";
			}
		}
		std::cout<<std::endl;
	}
}

void cl_ofdm::ZF_channel_estimator(std::complex <double>*in)
{
	int pilot_index=0;
	for(int i=0;i<Nsymb;i++)
	{
		for(int j=0;j<Nc;j++)
		{
			if((ofdm_frame+i*Nc+j)->type==PILOT)
			{
				(estimated_channel+i*Nc+j)->status=MEASURED;
				(estimated_channel+i*Nc+j)->value=*(in+i*Nc+j)/pilot_configurator.sequence[pilot_index];
				pilot_index++;
			}
			else
			{
				(estimated_channel+i*Nc+j)->status=UNKNOWN;
				(estimated_channel+i*Nc+j)->value=0;
			}
		}
	}

	for(int j=0;j<Nc;j++)
	{
		if(j%this->pilot_configurator.Dx==0)
		{
			interpolate_linear_col(estimated_channel,Nc,Nsymb,j);
		}
		else if(j==Nc-1)
		{
			interpolate_linear_col(estimated_channel,Nc,Nsymb,j);
		}
	}

	for(int j=0;j<Nc;j+=this->pilot_configurator.Dx)
	{
		if(j+this->pilot_configurator.Dx<Nc)
		{
			interpolate_bilinear_matrix(estimated_channel,Nc,Nsymb,j,j+this->pilot_configurator.Dx,0,Nsymb-1);
		}
		else if(j!=Nc-1)
		{
			interpolate_bilinear_matrix(estimated_channel,Nc,Nsymb,j,Nc-1,0,Nsymb-1);
		}
	}
/*
 * Ref: R. Lucky, “The adaptive equalizer,” IEEE Signal Processing Magazine, vol. 23, no. 3, pp. 104–107, 2006.
 */
}

void cl_ofdm::LS_channel_estimator(std::complex <double>*in)
{
	std::complex <double> pilot_data[Nsymb*Nc]={std::complex <double> (0,0)};

	int pilot_index=0;
	for(int i=0;i<Nsymb;i++)
	{
		for(int j=0;j<Nc;j++)
		{
			if((ofdm_frame+i*Nc+j)->type==PILOT)
			{
				*(pilot_data+i*Nc+j)=pilot_configurator.sequence[pilot_index];
				pilot_index++;
			}
			else
			{
				(estimated_channel+i*Nc+j)->status=UNKNOWN;
				(estimated_channel+i*Nc+j)->value=0;
			}

		}
	}

	int nPilots=0;
	std::complex <double> ch_tmp;

	for(int j=0;j<Nc;j++)
	{
		for(int i=0;i<Nsymb;i++)
		{
			if((ofdm_frame+i*Nc+j)->type!=PILOT)
			{
				continue;
			}

			int window_vertical_start=i-LS_window_hight/2;
			int window_vertical_end=i+LS_window_hight/2;

			int window_horizontal_start=j-LS_window_width/2;
			int window_horizontal_end=j+LS_window_width/2;

			nPilots=0;
			for(int k=window_vertical_start;k<=window_vertical_end;k++)
			{
				if(k<0 || k>=Nsymb)
				{
					continue;
				}
				for(int l=window_horizontal_start;l<=window_horizontal_end;l++)
				{
					if(l<0 || l>=Nc)
					{
						continue;
					}

					if((ofdm_frame+k*Nc+l)->type==PILOT)
					{
						nPilots++;
					}
				}
			}

			std::complex <double> x[nPilots], y[nPilots];

			int x_y_index=0;
			for(int k=window_vertical_start;k<=window_vertical_end;k++)
			{
				if(k<0 || k>=Nsymb)
				{
					continue;
				}
				for(int l=window_horizontal_start;l<=window_horizontal_end;l++)
				{

					if(l<0 || l>=Nc)
					{
						continue;
					}
					if((ofdm_frame+k*Nc+l)->type==PILOT)
					{
						x[x_y_index]=*(pilot_data+k*Nc+l);
						y[x_y_index]=*(in+k*Nc+l);
						x_y_index++;
					}
				}
			}
			//ch_tmp=(x.transpose *x).inverse * x.transpose * y
			matrix_multiplication(x,nPilots,1,x,1,nPilots,&ch_tmp);
			ch_tmp=1.0/ch_tmp;
			for(int m=0;m<nPilots;m++)
			{
				x[m]*=ch_tmp;
			}
			matrix_multiplication(x,nPilots,1,y,1,nPilots,&ch_tmp);

			(estimated_channel+i*Nc+j)->status=MEASURED;
			(estimated_channel+i*Nc+j)->value=ch_tmp;
		}
	}


	for(int j=0;j<Nc;j++)
	{
		if(j%this->pilot_configurator.Dx==0)
		{
			interpolate_linear_col(estimated_channel,Nc,Nsymb,j);
		}
		else if(j==Nc-1)
		{
			interpolate_linear_col(estimated_channel,Nc,Nsymb,j);
		}
	}

	for(int j=0;j<Nc;j+=this->pilot_configurator.Dx)
	{
		if(j+this->pilot_configurator.Dx<Nc)
		{
			interpolate_bilinear_matrix(estimated_channel,Nc,Nsymb,j,j+this->pilot_configurator.Dx,0,Nsymb-1);
		}
		else if(j!=Nc-1)
		{
			interpolate_bilinear_matrix(estimated_channel,Nc,Nsymb,j,Nc-1,0,Nsymb-1);
		}
	}
/*
 * Ref J. . -J. van de Beek, O. Edfors, M. Sandell, S. K. Wilson and P. O. Borjesson, "On channel estimation in OFDM systems," 1995 IEEE 45th Vehicular Technology Conference. Countdown to the Wireless Twenty-First Century, Chicago, IL, USA, 1995, pp. 815-819 vol.2, doi: 10.1109/VETEC.1995.504981.
 */
}

void cl_ofdm::CPE_correction(std::complex<double>* in)
{
	if (Nsymb <= 0 || Nc <= 0) return;

	int Dy = pilot_configurator.Dy;
	if (Dy <= 0) return;

	// Estimate residual frequency offset from pilot phase rotation.
	// For each subcarrier column, adjacent pilot rows (separated by Dy
	// symbols) give a phase-change measurement. Averaging all such pairs
	// across the frame gives a robust estimate of the per-symbol phase
	// rate, which is then removed from the received data BEFORE channel
	// estimation. This prevents LS window phase cancellation.
	//
	// With NB (Nc=10, Dy=3): ~320 pilot pairs → very robust even at low SNR.
	// With WB (Nc=50, Dy=3): ~800+ pairs → marginal extra improvement.

	// Per-column state: last pilot row and raw H value
	int prev_row[Nc];                    // VLA, Nc <= 50
	std::complex<double> prev_H[Nc];     // VLA
	for (int j = 0; j < Nc; j++) prev_row[j] = -1;

	std::complex<double> dH_sum(0, 0);
	int dH_count = 0;

	int pilot_index = 0;
	for (int i = 0; i < Nsymb; i++)
	{
		for (int j = 0; j < Nc; j++)
		{
			if ((ofdm_frame + i * Nc + j)->type == PILOT)
			{
				std::complex<double> X = pilot_configurator.sequence[pilot_index];
				std::complex<double> H_raw = *(in + i * Nc + j) / X;

				if (prev_row[j] >= 0 && (i - prev_row[j]) == Dy)
				{
					dH_sum += H_raw * std::conj(prev_H[j]);
					dH_count++;
				}

				prev_row[j] = i;
				prev_H[j] = H_raw;
				pilot_index++;
			}
		}
	}

	if (dH_count < 2) {
		return;
	}

	double phase_per_Dy = std::arg(dH_sum);
	double phase_rate = phase_per_Dy / Dy;    // radians per symbol

	// Skip correction if negligible (< 0.1 degree/symbol)
	if (std::abs(phase_rate) < 0.00175) {
		return;
	}

	// Remove linear phase rotation from all symbols (symbol 0 = reference)
	for (int i = 0; i < Nsymb; i++)
	{
		std::complex<double> correction = std::exp(std::complex<double>(0, -phase_rate * i));
		for (int j = 0; j < Nc; j++)
		{
			*(in + i * Nc + j) *= correction;
		}
	}
}

void cl_ofdm::restore_channel_amplitude()
{
	for(int i=0;i<Nsymb;i++)
	{
		for(int j=0;j<Nc;j++)
		{
			*(estimated_channel_without_amplitude_restoration+i*Nc+j)=*(estimated_channel+i*Nc+j);
			(estimated_channel+i*Nc+j)->value=set_complex(1, get_angle((estimated_channel+i*Nc+j)->value));
		}
	}
/*
 * Ref: F. Jerji and C. Akamine, "Enhanced ZF and LS channel estimators for OFDM with MPSK modulation," 2024 IEEE International Symposium on Broadband Multimedia Systems and Broadcasting (BMSB).
 */
}
void cl_ofdm::automatic_gain_control(std::complex <double>*in)
{
	int pilot_index=0;
	double pilot_amp=0;
	double agc=0;
	for(int i=0;i<Nsymb;i++)
	{
		for(int j=0;j<Nc;j++)
		{
			if((ofdm_frame+i*Nc+j)->type==PILOT)
			{
				pilot_amp+=get_amplitude(*(in+i*Nc+j));
				pilot_index++;
			}
		}
	}
	pilot_amp/=pilot_index;
	agc=pilot_configurator.boost/pilot_amp;

	for(int i=0;i<Nsymb;i++)
	{
		for(int j=0;j<Nc;j++)
		{
			*(in+i*Nc+j)*=agc;
		}

	}
}

double cl_ofdm::measure_variance(std::complex <double>*in)
{
	double variance=0;
	int pilot_index=0;
	std::complex <double> diff;
	for(int i=0;i<Nsymb;i++)
	{
		for(int j=0;j<Nc;j++)
		{
			if((ofdm_frame+i*Nc+j)->type==PILOT)
			{
				diff=*(in+i*Nc+j) -pilot_configurator.sequence[pilot_index];
				pilot_index++;
				variance+=pow(diff.real(),2)+pow(diff.imag(),2);
			}
		}

	}
	variance/=(double)pilot_index;

	return variance;
}

double cl_ofdm::measure_signal_stregth(std::complex <double>*in, int nItems)
{
	double signal_stregth=0;
	double signal_stregth_dbm=0;
	std::complex <double> value;

	for(int i=0;i<nItems;i++)
	{
		value=*(in+i);
		signal_stregth+=pow(value.real(),2)+pow(value.imag(),2);
	}
	signal_stregth/=nItems;

	signal_stregth_dbm=10.0*log10((signal_stregth)/0.001);

	return signal_stregth_dbm;
}

st_power_measurment cl_ofdm::measure_signal_power_avg_papr(double*in, int nItems)
{
	st_power_measurment power_measurment;
	power_measurment.avg=0;
	power_measurment.max=0;
	power_measurment.papr_db=0;
	double power_tmp;

	for(int i=0;i<nItems;i++)
	{
		power_tmp=pow(*(in+i),2);
		power_measurment.avg+=power_tmp;
		if(power_tmp>power_measurment.max)
		{
			power_measurment.max=power_tmp;
		}
	}
	power_measurment.avg/=nItems;

	power_measurment.papr_db=10.0*log10(power_measurment.max/power_measurment.avg);

	return power_measurment;
}

void cl_ofdm::peak_clip(double *in, int nItems, double papr)
{
	double power_measurment_avg=0;
	double power_tmp=0;
	double peak_allowed=0;
	for(int i=0;i<nItems;i++)
	{
		power_tmp=pow(*(in+i),2);
		power_measurment_avg+=power_tmp;
	}
	power_measurment_avg/=nItems;

	peak_allowed=sqrt(power_measurment_avg*pow(10,papr/10.0));

	for(int i=0;i<nItems;i++)
	{
		if(*(in+i)>0 && *(in+i)>peak_allowed)
		{
			*(in+i)=peak_allowed;
		}

		if(*(in+i)<0 && *(in+i)< -peak_allowed)
		{
			*(in+i)=-peak_allowed;
		}
	}

}

void cl_ofdm::peak_clip(std::complex <double> *in, int nItems, double papr)
{
	double power_measurment_avg=0;
	double power_tmp=0;
	double peak_allowed=0;
	std::complex <double> value;
	for(int i=0;i<nItems;i++)
	{
		value=*(in+i);
		power_tmp=pow(value.real(),2)+pow(value.imag(),2);
		power_measurment_avg+=power_tmp;
	}
	power_measurment_avg/=nItems;
	peak_allowed=power_measurment_avg*pow(10,papr/10.0);

	for(int i=0;i<nItems;i++)
	{
		value=*(in+i);
		power_tmp=pow(value.real(),2)+pow(value.imag(),2);

		if(power_tmp>peak_allowed)
		{
			*(in+i)= set_complex(sqrt(peak_allowed), get_angle(*(in+i)));
		}
	}

}

double cl_ofdm::measure_SNR(std::complex <double>*in_s, std::complex <double>*in_n, int nItems)
{
	double variance=0;
	double SNR=0;
	std::complex <double> diff;
	for(int i=0;i<nItems;i++)
	{
		diff=*(in_n+i)-*(in_s+i);
		variance+=pow(diff.real(),2)+pow(diff.imag(),2);
	}
	variance/=nItems;
	SNR=-10.0*log10(variance);
	return SNR;
}

void cl_ofdm::channel_equalizer(std::complex <double>* in, std::complex <double>* out)
{
	for(int i=0;i<Nsymb;i++)
	{
		for(int j=0;j<Nc;j++)
		{
			*(out+i*Nc+j)=*(in+i*Nc+j) / (estimated_channel+i*Nc+j)->value;
			(estimated_channel+i*Nc+j)->status=UNKNOWN;
		}
	}
}
void cl_ofdm::channel_equalizer_without_amplitude_restoration(std::complex <double>* in,std::complex <double>* out)
{
	for(int i=0;i<Nsymb;i++)
	{
		for(int j=0;j<Nc;j++)
		{
			*(out+i*Nc+j)=*(in+i*Nc+j) / (estimated_channel_without_amplitude_restoration+i*Nc+j)->value;
		}
	}
}

int cl_ofdm::time_sync(std::complex <double>*in, int size, int interpolation_rate, int location_to_return)
{

	double corss_corr=0;
	double norm_a=0;
	double norm_b=0;

	int *corss_corr_loc=new int[size];
	double *corss_corr_vals=new double[size];
	int return_val;

	std::complex <double> *a_c, *b_c;

	for(int i=0;i<size;i++)
	{
		corss_corr_loc[i]=-1;
		corss_corr_vals[i]=0;
	}

	for(int i=0;i<size-(this->Ngi+this->Nfft)*interpolation_rate;i++)
	{
		a_c=in+i;
		b_c=in+i+this->Nfft*interpolation_rate;
		corss_corr=0;
		norm_a=0;
		norm_b=0;
		for(int j=0;j<Nsymb+preamble_configurator.Nsymb;j++)
		{
			if(j<time_sync_Nsymb)
			{
				for(int m=0;m<this->Ngi*interpolation_rate;m++)
				{
					corss_corr+=a_c[m+j*(this->Ngi+this->Nfft)*interpolation_rate].real()*b_c[m+j*(this->Ngi+this->Nfft)*interpolation_rate].real();
					norm_a+=a_c[m+j*(this->Ngi+this->Nfft)*interpolation_rate].real()*a_c[m+j*(this->Ngi+this->Nfft)*interpolation_rate].real();
					norm_b+=b_c[m+j*(this->Ngi+this->Nfft)*interpolation_rate].real()*b_c[m+j*(this->Ngi+this->Nfft)*interpolation_rate].real();

					corss_corr+=a_c[m+j*(this->Ngi+this->Nfft)*interpolation_rate].imag()*b_c[m+j*(this->Ngi+this->Nfft)*interpolation_rate].imag();
					norm_a+=a_c[m+j*(this->Ngi+this->Nfft)*interpolation_rate].imag()*a_c[m+j*(this->Ngi+this->Nfft)*interpolation_rate].imag();
					norm_b+=b_c[m+j*(this->Ngi+this->Nfft)*interpolation_rate].imag()*b_c[m+j*(this->Ngi+this->Nfft)*interpolation_rate].imag();
				}
			}
		}
		corss_corr=corss_corr/sqrt(norm_a*norm_b);
		corss_corr_vals[i]=corss_corr;
		corss_corr_loc[i]=i;
	}
	double tmp;
	int tmp_int;
	for(int i=0;i<size-(this->Ngi+this->Nfft)*interpolation_rate-1;i++)
	{
		for(int j=0;j<size-(this->Ngi+this->Nfft)*interpolation_rate-1;j++)
		{
			if (corss_corr_vals[j]<corss_corr_vals[j+1])
			{
				tmp=corss_corr_vals[j];
				corss_corr_vals[j]=corss_corr_vals[j+1];
				corss_corr_vals[j+1]=tmp;

				tmp_int=corss_corr_loc[j];
				corss_corr_loc[j]=corss_corr_loc[j+1];
				corss_corr_loc[j+1]=tmp_int;
			}
		}
	}
	return_val=corss_corr_loc[location_to_return];
	if(corss_corr_loc!=NULL)
	{
		delete[] corss_corr_loc;
	}
	if(corss_corr_vals!=NULL)
	{
		delete[] corss_corr_vals;
	}
	return return_val;
}

int cl_ofdm::time_sync_preamble(std::complex <double>*in, int size, int interpolation_rate, int location_to_return, int step, int nTrials_max)
{
	double corss_corr=0;
	double norm_a=0;
	double norm_b=0;

	// Grow-as-needed correlation buffers (shared with time_sync_preamble_with_metric)
	if(size > tsync_corr_size)
	{
		if(tsync_corr_loc!=NULL) delete[] tsync_corr_loc;
		if(tsync_corr_vals!=NULL) delete[] tsync_corr_vals;
		tsync_corr_loc = new int[size];
		tsync_corr_vals = new double[size];
		tsync_corr_size = size;
	}
	int *corss_corr_loc = tsync_corr_loc;
	double *corss_corr_vals = tsync_corr_vals;
	int return_val;


	std::complex <double> *a_c, *b_c;

	for(int i=0;i<size;i++)
	{
		corss_corr_loc[i]=-1;
		corss_corr_vals[i]=0;
	}

	int data_len = preamble_configurator.Nsymb*(this->Ngi+this->Nfft)*interpolation_rate;
	if(data_len > tsync_data_size)
	{
		if(tsync_data!=NULL) delete[] tsync_data;
		tsync_data = new std::complex<double>[data_len];
		tsync_data_size = data_len;
	}
	std::complex <double> *data = tsync_data;

	for(int i=0;i<size-preamble_configurator.Nsymb*(this->Ngi+this->Nfft)*interpolation_rate;i+=step)
	{
		for(int k=0;k<preamble_configurator.Nsymb*(this->Ngi+this->Nfft)*interpolation_rate;k++)
		{
			data[k]=*(in+i+k);
		}

		corss_corr=0;
		norm_a=0;
		norm_b=0;
		// GI-only correlation: the guard interval is a copy of the last Ngi
		// samples of the OFDM symbol, so GI[n] correlates perfectly with
		// symbol[Nfft+n] for real preambles regardless of which FFT bins
		// are active. The original Schmidl-Cox half-symbol correlation
		// (x[n] vs x[n+N/2]) requires only even-indexed subcarriers,
		// which Mercury's preamble does not satisfy (it uses all Nc bins).
		// Using GI-only gives metric ~1.0 for real preambles, ~0 for noise.
		for(int l=0;l<preamble_configurator.Nsymb;l++)
		{
			a_c=data+l*(this->Ngi+this->Nfft)*interpolation_rate;
			b_c=data+l*(this->Ngi+this->Nfft)*interpolation_rate+this->Nfft*interpolation_rate;

			for(int m=0;m<this->Ngi*interpolation_rate;m++)
			{
				corss_corr+=a_c[m].real()*b_c[m].real();
				norm_a+=a_c[m].real()*a_c[m].real();
				norm_b+=b_c[m].real()*b_c[m].real();

				corss_corr+=a_c[m].imag()*b_c[m].imag();
				norm_a+=a_c[m].imag()*a_c[m].imag();
				norm_b+=b_c[m].imag()*b_c[m].imag();
			}
		}

		if(norm_a < 0.001 || norm_b < 0.001)
			corss_corr = 0.0;
		else
			corss_corr=corss_corr/sqrt(norm_a*norm_b);
		corss_corr_vals[i]=corss_corr;
		corss_corr_loc[i]=i;
	}

	// Clamp location_to_return to valid range to prevent reading uninitialized sort entries
	if(location_to_return >= nTrials_max)
		location_to_return = nTrials_max - 1;

	for(int j=0;j<nTrials_max;j++)
	{
		corss_corr_loc[j]=j;
		for(int i=j+1;i<size;i++)
		{
			if (corss_corr_vals[i]>corss_corr_vals[j])
			{
				corss_corr_vals[j]=corss_corr_vals[i];
				corss_corr_loc[j]=i;
			}
		}
	}

	return_val=corss_corr_loc[location_to_return];
	return return_val;
/*
 * 	Ref: T. M. Schmidl and D. C. Cox, "Robust frequency and timing synchronization for OFDM," in IEEE Transactions on Communications, vol. 45, no. 12, pp. 1613-1621, Dec. 1997, doi: 10.1109/26.650240.
 *
 */
}

TimeSyncResult cl_ofdm::time_sync_preamble_with_metric(std::complex <double>*in, int size, int interpolation_rate, int location_to_return, int step, int nTrials_max)
{
	/*
	 * Same as time_sync_preamble() but also returns the correlation metric.
	 * This allows the caller to assess the quality of the time sync detection.
	 * A high correlation (>0.7) indicates strong preamble detection.
	 */
	double corss_corr=0;
	double norm_a=0;
	double norm_b=0;
	double max_correlation = 0.0;

	TimeSyncResult result;
	result.delay = 0;
	result.correlation = 0.0;

	// Grow-as-needed correlation buffers
	if(size > tsync_corr_size)
	{
		if(tsync_corr_loc!=NULL) delete[] tsync_corr_loc;
		if(tsync_corr_vals!=NULL) delete[] tsync_corr_vals;
		tsync_corr_loc = new int[size];
		tsync_corr_vals = new double[size];
		tsync_corr_size = size;
	}
	int *corss_corr_loc = tsync_corr_loc;
	double *corss_corr_vals = tsync_corr_vals;

	std::complex <double> *a_c, *b_c;

	for(int i=0;i<size;i++)
	{
		corss_corr_loc[i]=-1;
		corss_corr_vals[i]=0;
	}

	int data_len = preamble_configurator.Nsymb*(this->Ngi+this->Nfft)*interpolation_rate;
	if(data_len > tsync_data_size)
	{
		if(tsync_data!=NULL) delete[] tsync_data;
		tsync_data = new std::complex<double>[data_len];
		tsync_data_size = data_len;
	}
	std::complex <double> *data = tsync_data;

	for(int i=0;i<size-preamble_configurator.Nsymb*(this->Ngi+this->Nfft)*interpolation_rate;i+=step)
	{
		for(int k=0;k<preamble_configurator.Nsymb*(this->Ngi+this->Nfft)*interpolation_rate;k++)
		{
			data[k]=*(in+i+k);
		}

		corss_corr=0;
		norm_a=0;
		norm_b=0;
		// GI + half-symbol correlation.
		// GI: correlate cyclic prefix with end of FFT symbol (Ngi samples/symbol).
		// Halfsym: even-only subcarrier preamble has period Nfft/2 in time domain,
		// so first half of FFT window correlates with second half (Nfft/2 samples).
		// Combined: 576 samples/symbol × 4 symbols = 2304 total — robust enough
		// to reject false peaks from data symbols (which lack half-symbol periodicity).
		for(int l=0;l<preamble_configurator.Nsymb;l++)
		{
			a_c=data+l*(this->Ngi+this->Nfft)*interpolation_rate;
			b_c=data+l*(this->Ngi+this->Nfft)*interpolation_rate+this->Nfft*interpolation_rate;

			for(int m=0;m<this->Ngi*interpolation_rate;m++)
			{
				corss_corr+=a_c[m].real()*b_c[m].real();
				norm_a+=a_c[m].real()*a_c[m].real();
				norm_b+=b_c[m].real()*b_c[m].real();

				corss_corr+=a_c[m].imag()*b_c[m].imag();
				norm_a+=a_c[m].imag()*a_c[m].imag();
				norm_b+=b_c[m].imag()*b_c[m].imag();
			}

			a_c=data+l*(this->Ngi+this->Nfft)*interpolation_rate+this->Ngi*interpolation_rate;
			b_c=data+l*(this->Ngi+this->Nfft)*interpolation_rate+(this->Ngi+this->Nfft/2)*interpolation_rate;

			for(int m=0;m<(this->Nfft/2)*interpolation_rate;m++)
			{
				corss_corr+=a_c[m].real()*b_c[m].real();
				norm_a+=a_c[m].real()*a_c[m].real();
				norm_b+=b_c[m].real()*b_c[m].real();

				corss_corr+=a_c[m].imag()*b_c[m].imag();
				norm_a+=a_c[m].imag()*a_c[m].imag();
				norm_b+=b_c[m].imag()*b_c[m].imag();
			}
		}

		// Norm threshold: VB-Cable silence has amplitude ~1e-10 (nonzero).
		// Norms accumulate to ~1e-18 and the ratio produces unstable metrics
		// (up to 0.93) that can beat real preamble peaks. Use threshold
		// instead of exact == 0.0 to suppress these degenerate cases.
		if(norm_a < 0.001 || norm_b < 0.001)
			corss_corr = 0.0;
		else
			corss_corr=corss_corr/sqrt(norm_a*norm_b);
		corss_corr_vals[i]=corss_corr;
		corss_corr_loc[i]=i;
	}

	// Clamp location_to_return to valid range to prevent reading uninitialized sort entries
	if(location_to_return >= nTrials_max)
		location_to_return = nTrials_max - 1;

	for(int j=0;j<nTrials_max;j++)
	{
		corss_corr_loc[j]=j;
		for(int i=j+1;i<size;i++)
		{
			if (corss_corr_vals[i]>corss_corr_vals[j])
			{
				corss_corr_vals[j]=corss_corr_vals[i];
				corss_corr_loc[j]=i;
			}
		}
	}

	result.delay = corss_corr_loc[location_to_return];
	// Get the correlation value at the returned location
	max_correlation = corss_corr_vals[location_to_return];
	result.correlation = max_correlation;

	return result;
}

TimeSyncResult cl_ofdm::time_sync_preamble_halfsym(std::complex<double>* in, int size, int interpolation_rate, int step)
{
	/*
	 * Schmidl-Cox half-symbol preamble detection for wideband OFDM.
	 *
	 * Even-subcarrier preamble has period L = Nfft/2 in time domain:
	 * r(d+m) = r(d+m+L) for all m within each symbol. Data symbols lack
	 * this periodicity. Sliding L-sample windows give |P|²/R² ≈ 1.0 at
	 * preamble, ≈ 0.0 at data, at ANY sample position (no GI alignment
	 * needed). GI extends the periodicity through the cyclic prefix.
	 *
	 * Uses magnitude-squared metric for phase-rotation invariance:
	 *   P = sum conj(r(d+m)) * r(d+m+L)
	 *   R = sum |r(d+m+L)|^2
	 *   M = |P|^2 / R^2
	 */
	int L = (this->Nfft / 2) * interpolation_rate;
	int Nofdm = (this->Ngi + this->Nfft) * interpolation_rate;
	int nsym = preamble_configurator.Nsymb;
	int pream_len = nsym * Nofdm;

	TimeSyncResult result;
	result.delay = 0;
	result.correlation = 0.0;

	double best_weighted = -1.0;
	double best_normalized = 0.0;
	int best_pos = 0;

	for(int d = 0; d <= size - pream_len; d += step)
	{
		double P_real = 0.0, P_imag = 0.0;
		double A2 = 0.0, R = 0.0;

		for(int sym = 0; sym < nsym; sym++)
		{
			int base = d + sym * Nofdm;
			for(int m = 0; m < L; m++)
			{
				double ar = in[base + m].real();
				double ai = in[base + m].imag();
				double br = in[base + m + L].real();
				double bi = in[base + m + L].imag();

				// conj(a) * b = (ar*br + ai*bi) + j(ar*bi - ai*br)
				P_real += ar * br + ai * bi;
				P_imag += ar * bi - ai * br;

				A2 += ar * ar + ai * ai;
				R += br * br + bi * bi;
			}
		}

		// Correlation coefficient: M = |P|² / (A² · R)
		// Bounded [0, 1] by Cauchy-Schwarz. Normalizing by both halves
		// prevents metric explosion at signal/silence boundaries where
		// A² >> R (first half has signal, second half is silence).
		double metric = 0.0;
		double denom = A2 * R;
		if(denom > 1e-20)
			metric = (P_real * P_real + P_imag * P_imag) / denom;

		// Energy-weighted selection: use metric * energy to pick best position.
		// Pure normalized metric gives false peaks on silence (VB-Cable digital
		// silence has energy ~1e-12, making denom ~1e-19 which barely exceeds
		// 1e-20, producing unstable correlation ratios ~0.1-1.0).
		// Weighting by (A2+R) ensures silence (energy ~0) never beats real
		// signal, while preserving correct detection among signal positions.
		double weighted = metric * (A2 + R);
		if(weighted > best_weighted)
		{
			best_weighted = weighted;
			best_normalized = metric;
			best_pos = d;
		}
	}

	result.delay = best_pos;
	result.correlation = best_normalized;
	return result;
}

TimeSyncResult cl_ofdm::time_sync_preamble_fft(
	std::complex<double>* baseband_interp, int buffer_size_interp,
	int interpolation_rate, int preamble_nSymb)
{
	/*
	 * FFT-based preamble detection for narrowband OFDM.
	 *
	 * Coarse search at GI-period steps. Per-bin coherent across symbols
	 * (timing-dependent phase is constant per bin across symbols), non-coherent
	 * across bins (avoids cross-bin phase spread from timing offset).
	 *
	 * PREAMBLE-SPECIFIC: correlates against known preamble subcarrier values.
	 * Data symbols produce metric ≈ 1 (random correlation), preamble ≈ Nsym.
	 *
	 * Normalized metric = Σ|bin_accum|² / Σ|FFT_bin|² × |P|²
	 *   ≈ preamble_nSymb at preamble (coherent gain)
	 *   ≈ 1 at noise or data (random walk)
	 * Threshold of ~2 gives reliable discrimination.
	 *
	 * Fine timing is handled by GI correlation in the caller (±1 symbol window).
	 */

	int Nofdm = Nfft + Ngi;
	int symbol_interp = Nofdm * interpolation_rate;
	int preamble_interp = preamble_nSymb * symbol_interp;
	int gi_interp = Ngi * interpolation_rate;

	// Precompute preamble bin list
	int n_preamble_bins_per_sym[16] = {};
	int preamble_bin_list[16][256];
	int preamble_bin_fft[16][256];

	for(int sym = 0; sym < preamble_nSymb && sym < 16; sym++)
	{
		int nb = 0;
		for(int k = 0; k < Nc; k++)
		{
			if(ofdm_preamble[sym * Nc + k].type == PREAMBLE)
			{
				int fft_bin;
				if(k < Nc / 2)
					fft_bin = k + Nfft - Nc / 2;
				else
					fft_bin = k - Nc / 2 + start_shift;
				preamble_bin_list[sym][nb] = k;
				preamble_bin_fft[sym][nb] = fft_bin;
				nb++;
			}
		}
		n_preamble_bins_per_sym[sym] = nb;
	}

	// Local FFT buffers
	std::complex<double> fft_in[256];
	std::complex<double> fft_out[256];

	// GI-period steps guarantee worst-case ±gi_interp/2 offset from symbol
	// boundary. Symbol-period steps are 17× coarser (1088 vs 64 at interp=4)
	// and cause ISI when the FFT window extends past the 64-sample GI.
	int search_step = gi_interp;
	int n_coarse = (buffer_size_interp - preamble_interp) / search_step;
	if(n_coarse < 0) n_coarse = 0;
	if(n_coarse > 1023) n_coarse = 1023;  // safety cap for local arrays

	double best_coarse_metric = -1.0;
	int best_coarse_pos = 0;
	double coarse_metrics[1024];  // stack alloc (n_coarse capped to 1023 above)

	std::complex<double> bin_accum[256];

	for(int pos = 0; pos <= n_coarse; pos++)
	{
		int sample_start = pos * search_step;

		int max_bins = n_preamble_bins_per_sym[0];
		for(int b = 0; b < max_bins; b++)
			bin_accum[b] = std::complex<double>(0.0, 0.0);

		for(int sym = 0; sym < preamble_nSymb; sym++)
		{
			int sym_start = sample_start + sym * symbol_interp;
			int fft_start = sym_start + gi_interp;

			if(fft_start + (Nfft - 1) * interpolation_rate >= buffer_size_interp)
				break;

			for(int k = 0; k < Nfft; k++)
				fft_in[k] = baseband_interp[fft_start + k * interpolation_rate];

			fft(fft_in, fft_out, Nfft);

			for(int b = 0; b < n_preamble_bins_per_sym[sym]; b++)
			{
				int sc = preamble_bin_list[sym][b];
				bin_accum[b] += fft_out[preamble_bin_fft[sym][b]] * std::conj(ofdm_preamble[sym * Nc + sc].value);
			}
		}

		double metric = 0.0;
		for(int b = 0; b < n_preamble_bins_per_sym[0]; b++)
			metric += std::norm(bin_accum[b]);

		coarse_metrics[pos] = metric;
		if(metric > best_coarse_metric)
		{
			best_coarse_metric = metric;
			best_coarse_pos = sample_start;
		}
	}

	// Prefer earliest position with metric >= 50% of max.
	// Preamble gives ~Nsymb² × E, data gives ~Nsymb × E (random walk).
	// At ratio 4:1, 50% of preamble max is 2× average data max —
	// data never reaches this, so the earliest preamble is always selected.
	// This prevents the FFT from jumping to a later frame when an earlier
	// preamble has slightly lower metric (e.g., timing offset effects).
	double early_threshold = best_coarse_metric * 0.5;
	for(int pos = 0; pos <= n_coarse; pos++)
	{
		if(coarse_metrics[pos] >= early_threshold)
		{
			best_coarse_metric = coarse_metrics[pos];
			best_coarse_pos = pos * search_step;
			break;
		}
	}

	// Normalize: re-FFT at best position and compute energy at preamble bins
	double energy = 0.0;
	{
		int sample_start = best_coarse_pos;
		for(int sym = 0; sym < preamble_nSymb; sym++)
		{
			int sym_start = sample_start + sym * symbol_interp;
			int fft_start = sym_start + gi_interp;

			if(fft_start + (Nfft - 1) * interpolation_rate >= buffer_size_interp)
				break;

			for(int k = 0; k < Nfft; k++)
				fft_in[k] = baseband_interp[fft_start + k * interpolation_rate];

			fft(fft_in, fft_out, Nfft);

			for(int b = 0; b < n_preamble_bins_per_sym[sym]; b++)
				energy += std::norm(fft_out[preamble_bin_fft[sym][b]]);
		}
	}

	TimeSyncResult result;
	result.delay = best_coarse_pos;
	result.correlation = (energy > 0.0) ? best_coarse_metric / energy : 0.0;
	return result;
}

TimeSyncResult cl_ofdm::time_sync_preamble_fft_fine(
	std::complex<double>* baseband_interp, int buffer_size_interp,
	int interpolation_rate, int preamble_nSymb,
	int coarse_pos, int search_half_window)
{
	/*
	 * FFT fine preamble detection + GI-only sample-level refinement.
	 *
	 * Stage 1: FFT search at half-GI steps in a narrow window around coarse_pos.
	 *          Same metric as time_sync_preamble_fft() but finer grid.
	 *          Resolves the "wrong symbol boundary" ambiguity that GI+halfsym
	 *          Phase 2 cannot solve (GI+halfsym gives 0.88-0.99 on ALL symbols).
	 *
	 * Stage 2: GI-only correlation at step=1 within ±gi_interp/2 of the FFT
	 *          fine position. Safe because FFT already confirmed the correct
	 *          symbol — GI just fine-tunes within the guard interval.
	 *
	 * Returns: sample-accurate position with FFT metric (the discriminating one).
	 */

	int Nofdm = Nfft + Ngi;
	int symbol_interp = Nofdm * interpolation_rate;
	int preamble_interp = preamble_nSymb * symbol_interp;
	int gi_interp = Ngi * interpolation_rate;

	// --- Precompute preamble bin list (same as time_sync_preamble_fft) ---
	int n_preamble_bins_per_sym[16] = {};
	int preamble_bin_list[16][256];
	int preamble_bin_fft[16][256];

	for(int sym = 0; sym < preamble_nSymb && sym < 16; sym++)
	{
		int nb = 0;
		for(int k = 0; k < Nc; k++)
		{
			if(ofdm_preamble[sym * Nc + k].type == PREAMBLE)
			{
				int fft_bin;
				if(k < Nc / 2)
					fft_bin = k + Nfft - Nc / 2;
				else
					fft_bin = k - Nc / 2 + start_shift;
				preamble_bin_list[sym][nb] = k;
				preamble_bin_fft[sym][nb] = fft_bin;
				nb++;
			}
		}
		n_preamble_bins_per_sym[sym] = nb;
	}

	std::complex<double> fft_in[256];
	std::complex<double> fft_out[256];
	std::complex<double> bin_accum[256];

	// --- Stage 1: FFT fine search at half-GI steps ---
	int fine_step = gi_interp / 2;
	if(fine_step < 1) fine_step = 1;

	int win_start = coarse_pos - search_half_window;
	if(win_start < 0) win_start = 0;
	int win_end = coarse_pos + search_half_window;
	if(win_end + preamble_interp > buffer_size_interp)
		win_end = buffer_size_interp - preamble_interp;
	if(win_end < win_start) win_end = win_start;

	int n_fine = (win_end - win_start) / fine_step;
	if(n_fine < 0) n_fine = 0;
	if(n_fine > 511) n_fine = 511;

	double best_fine_metric = -1.0;
	int best_fine_pos = coarse_pos;
	double fine_metrics[512];  // stack alloc (n_fine capped to 511 above)

	for(int idx = 0; idx <= n_fine; idx++)
	{
		int sample_start = win_start + idx * fine_step;

		int max_bins = n_preamble_bins_per_sym[0];
		for(int b = 0; b < max_bins; b++)
			bin_accum[b] = std::complex<double>(0.0, 0.0);

		for(int sym = 0; sym < preamble_nSymb; sym++)
		{
			int sym_start = sample_start + sym * symbol_interp;
			int fft_start = sym_start + gi_interp;

			if(fft_start + (Nfft - 1) * interpolation_rate >= buffer_size_interp)
				break;

			for(int k = 0; k < Nfft; k++)
				fft_in[k] = baseband_interp[fft_start + k * interpolation_rate];

			fft(fft_in, fft_out, Nfft);

			for(int b = 0; b < n_preamble_bins_per_sym[sym]; b++)
			{
				int sc = preamble_bin_list[sym][b];
				bin_accum[b] += fft_out[preamble_bin_fft[sym][b]] * std::conj(ofdm_preamble[sym * Nc + sc].value);
			}
		}

		double metric = 0.0;
		for(int b = 0; b < n_preamble_bins_per_sym[0]; b++)
			metric += std::norm(bin_accum[b]);

		fine_metrics[idx] = metric;
		if(metric > best_fine_metric)
		{
			best_fine_metric = metric;
			best_fine_pos = win_start + idx * fine_step;
		}
	}

	// Earliest-above-50%-of-max selection (same as coarse)
	double early_threshold = best_fine_metric * 0.5;
	for(int idx = 0; idx <= n_fine; idx++)
	{
		if(fine_metrics[idx] >= early_threshold)
		{
			best_fine_metric = fine_metrics[idx];
			best_fine_pos = win_start + idx * fine_step;
			break;
		}
	}

	// Normalize FFT metric (same as time_sync_preamble_fft)
	double energy = 0.0;
	{
		int sample_start = best_fine_pos;
		for(int sym = 0; sym < preamble_nSymb; sym++)
		{
			int sym_start = sample_start + sym * symbol_interp;
			int fft_start = sym_start + gi_interp;

			if(fft_start + (Nfft - 1) * interpolation_rate >= buffer_size_interp)
				break;

			for(int k = 0; k < Nfft; k++)
				fft_in[k] = baseband_interp[fft_start + k * interpolation_rate];

			fft(fft_in, fft_out, Nfft);

			for(int b = 0; b < n_preamble_bins_per_sym[sym]; b++)
				energy += std::norm(fft_out[preamble_bin_fft[sym][b]]);
		}
	}

	double fft_metric = (energy > 0.0) ? best_fine_metric / energy : 0.0;

	// --- Stage 2: GI-only refinement at step=1 within ±gi_interp/2 ---
	// GI correlation: correlate cyclic prefix (first Ngi samples) with the
	// matching end of the FFT window (last Ngi samples) across all preamble symbols.
	int gi_win_start = best_fine_pos - gi_interp / 2;
	if(gi_win_start < 0) gi_win_start = 0;
	int gi_win_end = best_fine_pos + gi_interp / 2;
	if(gi_win_end + preamble_interp > buffer_size_interp)
		gi_win_end = buffer_size_interp - preamble_interp;
	if(gi_win_end < gi_win_start) gi_win_end = gi_win_start;

	double best_gi_metric = -1.0;
	int best_gi_pos = best_fine_pos;

	for(int pos = gi_win_start; pos <= gi_win_end; pos++)
	{
		double corr = 0.0, na = 0.0, nb = 0.0;
		for(int sym = 0; sym < preamble_nSymb; sym++)
		{
			int base = pos + sym * symbol_interp;
			std::complex<double>* a = &baseband_interp[base];
			std::complex<double>* b = &baseband_interp[base + Nfft * interpolation_rate];
			for(int m = 0; m < gi_interp; m++)
			{
				corr += a[m].real() * b[m].real() + a[m].imag() * b[m].imag();
				na += a[m].real() * a[m].real() + a[m].imag() * a[m].imag();
				nb += b[m].real() * b[m].real() + b[m].imag() * b[m].imag();
			}
		}

		double gi_metric;
		if(na < 0.001 || nb < 0.001)
			gi_metric = -1.0;
		else
			gi_metric = corr / sqrt(na * nb);

		if(gi_metric > best_gi_metric)
		{
			best_gi_metric = gi_metric;
			best_gi_pos = pos;
		}
	}

	TimeSyncResult result;
	result.delay = best_gi_pos;
	result.correlation = fft_metric;  // Return FFT metric (the discriminating one)
	return result;
}

TimeSyncResult cl_ofdm::time_sync_preamble_matched(
	std::complex<double>* baseband_interp, int buffer_size_interp,
	int interpolation_rate, int preamble_nSymb)
{
	/*
	 * Matched-filter preamble detection: time-domain cross-correlation
	 * against FIR-round-tripped preamble template. Zero FFTs.
	 *
	 * Two phases:
	 *   Coarse: GI-period stride over search window. Uses first preamble
	 *           symbol only for speed. Per-symbol normalized correlation:
	 *           |conj(template) · signal|² / (E_template × E_signal) → [0,1].
	 *
	 *   Fine:   baseband-sample stride (step = interpolation_rate) within
	 *           ±gi_interp of coarse peak. All preamble symbols for accuracy.
	 *
	 * Metric convention: return avg_metric × preamble_nSymb so that
	 *   preamble ≈ Nsymb, data ≈ 0, threshold = 2.0.
	 */

	if (ofdm_corr_template == NULL || ofdm_corr_template_len <= 0)
	{
		TimeSyncResult r;
		r.delay = 0;
		r.correlation = 0.0;
		return r;
	}

	int Nofdm = Nfft + Ngi;
	int sym_interp = Nofdm * interpolation_rate;
	int preamble_interp = preamble_nSymb * sym_interp;
	int gi_interp = Ngi * interpolation_rate;
	int template_nsymb = ofdm_corr_template_nsymb;
	if (template_nsymb > preamble_nSymb) template_nsymb = preamble_nSymb;

	// ---- Coarse search: GI-period stride, ALL symbols ----
	// Uses all preamble symbols for discrimination. Single-symbol coarse
	// was insufficient: data can randomly correlate with one template symbol
	// at 0.4-0.5, causing the earliest-above-50% heuristic to pick false peaks.
	// All-symbol coarse: ~962K MACs (NB) — still 4x cheaper than FFT approach.
	int coarse_stride = gi_interp;
	int n_coarse = (buffer_size_interp - preamble_interp) / coarse_stride;
	if (n_coarse < 0) n_coarse = 0;

	double best_coarse_metric = -1.0;
	int best_coarse_pos = 0;

	// Stack buffer for metrics (earliest-above-50% selection needs all values)
	double* coarse_metrics = (double*)alloca((n_coarse + 1) * sizeof(double));

	for (int ci = 0; ci <= n_coarse; ci++)
	{
		int pos = ci * coarse_stride;

		double total_metric = 0.0;
		int valid_syms = 0;

		for (int k = 0; k < template_nsymb; k++)
		{
			int tmpl_offset = k * Nofdm;
			int rx_offset = pos + k * sym_interp;

			if (rx_offset + (Nofdm - 1) * interpolation_rate >= buffer_size_interp)
				break;

			double corr_re = 0.0, corr_im = 0.0;
			double e_rx = 0.0;

			for (int n = 0; n < Nofdm; n++)
			{
				std::complex<double> rx = baseband_interp[rx_offset + n * interpolation_rate];
				double t_re = ofdm_corr_template[tmpl_offset + n].real();
				double t_im = ofdm_corr_template[tmpl_offset + n].imag();

				corr_re += t_re * rx.real() + t_im * rx.imag();
				corr_im += t_im * rx.real() - t_re * rx.imag();
				e_rx += rx.real() * rx.real() + rx.imag() * rx.imag();
			}

			double denom = ofdm_corr_template_sym_energy[k] * e_rx;
			if (denom > 1e-30)
			{
				total_metric += (corr_re * corr_re + corr_im * corr_im) / denom;
				valid_syms++;
			}
		}

		double metric = (valid_syms > 0) ? total_metric / valid_syms : 0.0;
		coarse_metrics[ci] = metric;

		if (metric > best_coarse_metric)
		{
			best_coarse_metric = metric;
			best_coarse_pos = pos;
		}
	}

	// Earliest-above-50%-of-max selection (same heuristic as FFT detection)
	double early_threshold = best_coarse_metric * 0.5;
	int coarse_result_pos = best_coarse_pos;
	for (int ci = 0; ci <= n_coarse; ci++)
	{
		if (coarse_metrics[ci] >= early_threshold)
		{
			coarse_result_pos = ci * coarse_stride;
			break;
		}
	}

	// If no preamble detected at coarse level, return early
	if (best_coarse_metric < 0.15)
	{
		TimeSyncResult r;
		r.delay = coarse_result_pos;
		r.correlation = best_coarse_metric * preamble_nSymb;
		return r;
	}

	// ---- Fine search: all symbols, step = interpolation_rate, ±gi_interp ----
	int fine_start = coarse_result_pos - gi_interp;
	if (fine_start < 0) fine_start = 0;
	int fine_end = coarse_result_pos + gi_interp;
	if (fine_end + preamble_interp > buffer_size_interp)
		fine_end = buffer_size_interp - preamble_interp;
	if (fine_end < fine_start) fine_end = fine_start;

	double best_fine_metric = -1.0;
	int best_fine_pos = coarse_result_pos;

	for (int pos = fine_start; pos <= fine_end; pos += interpolation_rate)
	{
		double total_metric = 0.0;
		int valid_syms = 0;

		for (int k = 0; k < template_nsymb; k++)
		{
			int tmpl_offset = k * Nofdm;
			int rx_offset = pos + k * sym_interp;

			if (rx_offset + (Nofdm - 1) * interpolation_rate >= buffer_size_interp)
				break;

			double corr_re = 0.0, corr_im = 0.0;
			double e_rx = 0.0;

			for (int n = 0; n < Nofdm; n++)
			{
				std::complex<double> rx = baseband_interp[rx_offset + n * interpolation_rate];
				double t_re = ofdm_corr_template[tmpl_offset + n].real();
				double t_im = ofdm_corr_template[tmpl_offset + n].imag();

				corr_re += t_re * rx.real() + t_im * rx.imag();
				corr_im += t_im * rx.real() - t_re * rx.imag();
				e_rx += rx.real() * rx.real() + rx.imag() * rx.imag();
			}

			double denom = ofdm_corr_template_sym_energy[k] * e_rx;
			if (denom > 1e-30)
			{
				total_metric += (corr_re * corr_re + corr_im * corr_im) / denom;
				valid_syms++;
			}
		}

		double metric = (valid_syms > 0) ? total_metric / valid_syms : 0.0;
		if (metric > best_fine_metric)
		{
			best_fine_metric = metric;
			best_fine_pos = pos;
		}
	}

	TimeSyncResult result;
	result.delay = best_fine_pos;
	// Scale: avg per-symbol metric × nsymb. Preamble ≈ nsymb, data ≈ 0.
	result.correlation = best_fine_metric * preamble_nSymb;
	return result;
}

int cl_ofdm::time_sync_mfsk(std::complex<double>* baseband_interp, int buffer_size_interp,
                            int interpolation_rate, int preamble_nSymb,
                            const int* preamble_tones, int mfsk_M,
                            int nStreams, const int* stream_offsets,
                            int search_start_symb, double* out_metric)
{
	// MFSK preamble time sync: correlate against known preamble tone sequence.
	// Multi-stream: each preamble symbol has one tone per stream band.
	// Score = sum of (target energy / total energy) across preamble symbols.
	// search_start_symb: skip positions before this to avoid re-finding old preambles.

	int Nofdm = Nfft + Ngi;
	int sym_period_interp = Nofdm * interpolation_rate;
	int buffer_nsymb = buffer_size_interp / sym_period_interp;

	std::complex<double>* decimated_sym = work_buf_a;
	std::complex<double>* fft_out = work_buf_b;

	// Map preamble tone indices to FFT bin indices for each stream
	int preamble_bins[8][4]; // [MAX_PREAMBLE_SYMB][MAX_STREAMS]
	int half = Nc / 2;
	for (int p = 0; p < preamble_nSymb; p++)
	{
		for (int st = 0; st < nStreams; st++)
		{
			int subcarrier = stream_offsets[st] + preamble_tones[p % preamble_nSymb];
			int bin;
			if (subcarrier < half)
				bin = Nfft - half + subcarrier;
			else
				bin = start_shift + (subcarrier - half);
			preamble_bins[p][st] = bin;
		}
	}

	double best_metric = -1;
	int best_sym_idx = 0;

	int s_start = (search_start_symb > 0) ? search_start_symb : 0;
	for (int s = s_start; s <= buffer_nsymb - preamble_nSymb; s++)
	{
		double metric = 0;

		for (int p = 0; p < preamble_nSymb; p++)
		{
			int sym_idx = s + p;
			int offset = sym_idx * sym_period_interp + Ngi * interpolation_rate;
			if (offset + Nfft * interpolation_rate > buffer_size_interp)
				break;

			// Decimate and FFT this symbol
			for (int i = 0; i < Nfft; i++)
			{
				decimated_sym[i] = baseband_interp[offset + i * interpolation_rate];
			}
			fft(decimated_sym, fft_out, Nfft);

			// Energy in expected preamble tone bins (all streams)
			double e_target = 0;
			for (int st = 0; st < nStreams; st++)
			{
				int bin = preamble_bins[p][st];
				e_target += fft_out[bin].real() * fft_out[bin].real() +
				            fft_out[bin].imag() * fft_out[bin].imag();
			}

			// Total energy across all Nc bins
			double e_total = 0;
			for (int k = 0; k < Nc; k++)
			{
				int bk;
				if (k < half)
					bk = Nfft - half + k;
				else
					bk = start_shift + (k - half);
				double e = fft_out[bk].real() * fft_out[bk].real() +
				           fft_out[bk].imag() * fft_out[bk].imag();
				e_total += e;
			}

			if (e_total > 0)
				metric += e_target / e_total;
		}

		if (metric > best_metric)
		{
			best_metric = metric;
			best_sym_idx = s;
		}
	}

	double threshold = (Nc <= 10) ? preamble_nSymb * 0.3 : preamble_nSymb * 0.5;  // NB: lower threshold (FIR leakage)

	if (out_metric) *out_metric = best_metric;

	if (g_verbose)
	{
		printf("[MFSK-SYNC] best_metric=%.3f threshold=%.1f best_sym=%d buffer_nsymb=%d preamble_nSymb=%d Nc=%d M=%d\n",
			best_metric, threshold, best_sym_idx, buffer_nsymb, preamble_nSymb, Nc, mfsk_M);

		// Show energy distribution at best position
		if (best_sym_idx >= 0 && best_metric > 0.01)
		{
			for (int p = 0; p < preamble_nSymb; p++)
			{
				int sym_idx = best_sym_idx + p;
				int offset = sym_idx * sym_period_interp + Ngi * interpolation_rate;
				if (offset + Nfft * interpolation_rate > buffer_size_interp)
					break;

				for (int i = 0; i < Nfft; i++)
					decimated_sym[i] = baseband_interp[offset + i * interpolation_rate];
				fft(decimated_sym, fft_out, Nfft);

				double e_target = 0, e_total = 0;
				int half = Nc / 2;
				for (int st = 0; st < nStreams; st++)
				{
					int bin = preamble_bins[p][st];
					e_target += fft_out[bin].real() * fft_out[bin].real() +
					            fft_out[bin].imag() * fft_out[bin].imag();
				}
				for (int k = 0; k < Nc; k++)
				{
					int bk = (k < half) ? (Nfft - half + k) : (start_shift + (k - half));
					e_total += fft_out[bk].real() * fft_out[bk].real() +
					           fft_out[bk].imag() * fft_out[bk].imag();
				}
				printf("  p%d: tone=%d bin=%d e_target=%.3e e_total=%.3e ratio=%.3f\n",
					p, preamble_tones[p], preamble_bins[p][0], e_target, e_total,
					e_total > 0 ? e_target / e_total : 0.0);
			}
		}
		fflush(stdout);
	}

	if (best_metric < threshold)
		return -1;  // No valid preamble found

	int delay = best_sym_idx * sym_period_interp;

	return delay;
}

// NB MFSK preamble detection via waveform cross-correlation.
// Correlates the pre-generated baseband preamble template against the received
// baseband buffer. With L=2176 samples (8 symbols × 272), noise metric ~0.0005
// vs signal ~1.0, giving ~2000:1 discrimination vs ~4:1 for FFT energy method.
//
// Per-symbol correlation: each MFSK symbol has a single tone, so per-symbol
// |corr|² is phase-invariant under timing offset. Summing per-symbol metrics
// avoids the destructive interference that occurs when correlating multi-tone
// templates coherently (different tones rotate at different rates).
//
// Returns delay in interpolated samples, or -1 if no preamble found.
int cl_ofdm::time_sync_mfsk_corr(std::complex<double>* baseband_interp,
                                  int buffer_size_interp, int interpolation_rate,
                                  int search_start_symb, double* out_metric)
{
	if (mfsk_corr_template == NULL || mfsk_corr_template_len <= 0 || mfsk_corr_template_nsymb <= 0)
		return -1;

	int Nofdm = Nfft + Ngi;
	int sym_period_interp = Nofdm * interpolation_rate;
	int buffer_nsymb = buffer_size_interp / sym_period_interp;
	int template_nsymb = mfsk_corr_template_nsymb;

	int p1_start_interp = (search_start_symb > 0) ? search_start_symb * sym_period_interp : 0;
	int p1_end_interp = (buffer_nsymb - template_nsymb) * sym_period_interp;

	// Phase 1 oversampling: search at sub-symbol resolution to catch preambles
	// that fall between symbol boundaries. With M=4 NB, the preamble metric at
	// half-symbol misalignment drops to ~0.25 — comparable to data content false
	// peaks (~0.22). At 4× oversampling, max misalignment is 12.5% of a symbol,
	// keeping preamble metric > 0.76 — well above data content. (Bug #44)
	static const int P1_OVERSAMPLE = 4;
	int p1_step = sym_period_interp / P1_OVERSAMPLE;

	// Collect top-K Phase 1 candidates for multi-candidate Phase 2 evaluation.
	static const int P1_TOP_K = 8;
	struct { int pos_interp; double metric; } p1_candidates[P1_TOP_K];
	int n_candidates = 0;

	double best_p1_metric = -1.0;
	int best_p1_pos = -1;

	for (int base_interp = p1_start_interp; base_interp <= p1_end_interp; base_interp += p1_step)
	{
		// Check bounds: need all template symbols
		if (base_interp + (template_nsymb * Nofdm - 1) * interpolation_rate >= buffer_size_interp)
			break;

		double total_metric = 0.0;
		int valid_syms = 0;
		bool rejected = false;
		double per_sym_floor = 0.05;

		for (int k = 0; k < template_nsymb && !rejected; k++)
		{
			int tmpl_offset = k * Nofdm;
			int rx_offset = base_interp + k * sym_period_interp;

			double corr_re = 0.0, corr_im = 0.0;
			double e_rx_sym = 0.0;

			for (int n = 0; n < Nofdm; n++)
			{
				std::complex<double> rx = baseband_interp[rx_offset + n * interpolation_rate];
				double t_re = mfsk_corr_template[tmpl_offset + n].real();
				double t_im = mfsk_corr_template[tmpl_offset + n].imag();
				double r_re = rx.real();
				double r_im = rx.imag();

				corr_re += t_re * r_re + t_im * r_im;
				corr_im += t_im * r_re - t_re * r_im;

				e_rx_sym += r_re * r_re + r_im * r_im;
			}

			double corr_mag_sq = corr_re * corr_re + corr_im * corr_im;
			double denom = mfsk_corr_template_sym_energy[k] * e_rx_sym;
			if (denom > 1e-30)
			{
				double sym_metric = corr_mag_sq / denom;
				if (sym_metric < per_sym_floor)
				{
					rejected = true;
					break;
				}
				total_metric += sym_metric;
				valid_syms++;
			}
		}

		if (rejected) continue;

		double metric = (valid_syms > 0) ? total_metric / valid_syms : 0.0;

		// Insert into top-K sorted array (descending by metric)
		if (n_candidates < P1_TOP_K || metric > p1_candidates[n_candidates - 1].metric)
		{
			int insert_at = n_candidates < P1_TOP_K ? n_candidates : P1_TOP_K - 1;
			for (int t = 0; t < n_candidates && t < P1_TOP_K; t++)
			{
				if (metric > p1_candidates[t].metric)
				{
					insert_at = t;
					break;
				}
			}
			// Shift down
			int end = (n_candidates < P1_TOP_K) ? n_candidates : P1_TOP_K - 1;
			for (int u = end; u > insert_at; u--)
				p1_candidates[u] = p1_candidates[u - 1];
			p1_candidates[insert_at] = {base_interp, metric};
			if (n_candidates < P1_TOP_K) n_candidates++;
		}

		if (metric > best_p1_metric)
		{
			best_p1_metric = metric;
			best_p1_pos = base_interp;
		}
		// Early exit on first strong preamble: prefer the earliest detection
		// to avoid finding a commander resend whose frame overflows the buffer
		// while an earlier copy's frame fits. Real preambles score ~1.0,
		// noise ~0.004, so 0.5 has huge margin and won't false-trigger.
		if (metric > 0.5)
			break;
	}

	// Phase 1 must have found at least one non-rejected candidate for Phase 2
	// to have something to refine. If Phase 1 found nothing (all positions
	// rejected by per_sym_floor or empty buffer), skip Phase 2.
	if (best_p1_pos < 0)
	{
		if (out_metric) *out_metric = best_p1_metric;
		return -1;
	}

	// Phase 2: Fine timing refinement at base-rate resolution.
	// Run on ALL top-K Phase 1 candidates. With 4× oversampled Phase 1, the
	// preamble should be in top-K (metric > 0.76 vs data content ~0.22).
	// Phase 2 refines to exact sample position within ±1 symbol of each candidate.
	int search_half = sym_period_interp;
	int best_fine = best_p1_pos;
	double best_fine_metric = -1.0;
	int best_fine_cand = 0;

	for (int ci = 0; ci < n_candidates; ci++)
	{
		int coarse = p1_candidates[ci].pos_interp;

		for (int d = coarse - search_half; d <= coarse + search_half; d += interpolation_rate)
		{
			if (d < 0) continue;

			double total_metric = 0.0;
			int valid_syms = 0;
			bool out_of_bounds = false;

			for (int k = 0; k < template_nsymb && !out_of_bounds; k++)
			{
				int tmpl_off = k * Nofdm;
				int rx_base = d + k * sym_period_interp;

				if (rx_base + (Nofdm - 1) * interpolation_rate >= buffer_size_interp)
				{
					out_of_bounds = true;
					break;
				}

				double cr = 0.0, ci2 = 0.0, erx = 0.0;
				for (int n = 0; n < Nofdm; n++)
				{
					std::complex<double> rx = baseband_interp[rx_base + n * interpolation_rate];
					double t_re = mfsk_corr_template[tmpl_off + n].real();
					double t_im = mfsk_corr_template[tmpl_off + n].imag();
					cr += t_re * rx.real() + t_im * rx.imag();
					ci2 += t_im * rx.real() - t_re * rx.imag();
					erx += rx.real() * rx.real() + rx.imag() * rx.imag();
				}

				double denom = mfsk_corr_template_sym_energy[k] * erx;
				if (denom > 1e-30)
				{
					total_metric += (cr * cr + ci2 * ci2) / denom;
					valid_syms++;
				}
			}

			if (out_of_bounds) continue;
			double metric = (valid_syms > 0) ? total_metric / valid_syms : 0.0;
			if (metric > best_fine_metric)
			{
				best_fine_metric = metric;
				best_fine = d;
				best_fine_cand = ci;
			}
		}

		// Early exit: if this candidate already passed threshold, no need to
		// check lower-ranked candidates
		if (best_fine_metric > 0.5)
			break;
	}

	// Threshold: apply to Phase 2 refined metric (not Phase 1 coarse metric).
	// Real preamble after Phase 2 refinement: metric ≈ 1.0.
	// Random MFSK data at Phase 2 best: up to ~0.44 for M=4 (NB ROBUST_1).
	// Noise: ~0.004. Threshold 0.5 cleanly separates. (Bug #34, Bug #40, Bug #44)
	double threshold = 0.5;

	if (best_fine_metric < threshold)
	{
		if (out_metric) *out_metric = best_fine_metric;
		return -1;
	}

	if (out_metric) *out_metric = best_fine_metric;
	return best_fine;
}

// ACK pattern detection: slide window across buffer, accumulate E_target/E_total
// Returns best metric (0.0 = noise, up to ack_nsymb = perfect match)
double cl_ofdm::detect_ack_pattern(std::complex<double>* baseband_interp, int buffer_size_interp,
                                   int interpolation_rate, int ack_nsymb,
                                   const int* ack_tones, int ack_pattern_len,
                                   int tone_hop_step, int mfsk_M,
                                   int nStreams, const int* stream_offsets,
                                   int* out_matched)
{
	int Nofdm = Nfft + Ngi;
	int sym_period_interp = Nofdm * interpolation_rate;
	int buffer_nsymb = buffer_size_interp / sym_period_interp;

	if (buffer_nsymb < ack_nsymb) return 0.0;

	std::complex<double>* decimated_sym = work_buf_a;
	std::complex<double>* fft_out = work_buf_b;

	int half = Nc / 2;
	double best_metric = 0.0;
	int best_pos = -1;
	int best_matched = 0;

	for (int s = 0; s <= buffer_nsymb - ack_nsymb; s++)
	{
		double metric = 0;
		int matched = 0;

		for (int p = 0; p < ack_nsymb; p++)
		{
			int sym_idx = s + p;
			int offset = sym_idx * sym_period_interp + Ngi * interpolation_rate;
			if (offset + Nfft * interpolation_rate > buffer_size_interp)
				break;

			// Decimate and FFT
			for (int i = 0; i < Nfft; i++)
			{
				decimated_sym[i] = baseband_interp[offset + i * interpolation_rate];
			}
			fft(decimated_sym, fft_out, Nfft);

			// Which tone is expected at symbol p?
			int tone_base = ack_tones[p % ack_pattern_len];
			int actual_tone = (tone_base + p * tone_hop_step) % mfsk_M;

			// Order-aware detection: count this symbol only if the expected ACK tone
			// is the peak bin for ALL streams. Both streams transmit the same ACK
			// tone, so both should peak at the same bin. Using "any" causes high
			// false alarm rate for multi-stream modes (P(any)=1-(1-1/M)^nS ≈ 44%
			// for M=4, nS=2; P(all)=(1/M)^nS ≈ 6%).
			int streams_matched = 0;
			double e_target = 0;
			for (int st = 0; st < nStreams; st++)
			{
				int expected_subcarrier = stream_offsets[st] + actual_tone;
				int expected_bin;
				if (expected_subcarrier < half)
					expected_bin = Nfft - half + expected_subcarrier;
				else
					expected_bin = start_shift + (expected_subcarrier - half);
				double e_expected = fft_out[expected_bin].real() * fft_out[expected_bin].real() +
				                    fft_out[expected_bin].imag() * fft_out[expected_bin].imag();

				// Carrier image recovery (Bug #39): real passband → baseband
				// creates equal-energy mirrors at (Nfft - bin) % Nfft. For NB
				// (M=8, Nc=10), mirrors fall WITHIN the stream's M bins — the
				// FIR can't reject in-band images. Without recovery, the mirror
				// competes with the expected bin for "peak" status, giving ~50%
				// match rate. Fix: accept expected OR mirror as the peak bin.
				// Metric uses max(expected, mirror) to avoid inflating noise.
				int mirror_bin = (Nfft - expected_bin) % Nfft;
				double e_mirror = fft_out[mirror_bin].real() * fft_out[mirror_bin].real() +
				                  fft_out[mirror_bin].imag() * fft_out[mirror_bin].imag();
				e_target += e_expected + e_mirror;

				// Find peak bin (individual, not combined) among stream's M bins
				double peak_e = -1.0;
				int peak_bin = -1;
				for (int t = 0; t < mfsk_M; t++)
				{
					int sub = stream_offsets[st] + t;
					int b;
					if (sub < half)
						b = Nfft - half + sub;
					else
						b = start_shift + (sub - half);
					double e = fft_out[b].real() * fft_out[b].real() +
					           fft_out[b].imag() * fft_out[b].imag();
					if (e > peak_e)
					{
						peak_e = e;
						peak_bin = b;
					}
				}
				// Energy gate + carrier image: accept expected OR mirror as peak.
				// In silence (zeroed buffer), all bins have e=0 — energy gate
				// prevents 0>=0 false match.
				if (peak_e > 0 && (peak_bin == expected_bin || peak_bin == mirror_bin))
					streams_matched++;
			}

			if (streams_matched < nStreams)
				continue;

			matched++;

			// Total energy across all Nc bins
			double e_total = 0;
			for (int k = 0; k < Nc; k++)
			{
				int bk;
				if (k < half)
					bk = Nfft - half + k;
				else
					bk = start_shift + (k - half);
				double e = fft_out[bk].real() * fft_out[bk].real() +
				           fft_out[bk].imag() * fft_out[bk].imag();
				e_total += e;
			}

			if (e_total > 0)
				metric += e_target / e_total;
		}

		if (metric > best_metric)
		{
			best_metric = metric;
			best_pos = s;
			best_matched = matched;
		}
	}

	// Phase 2: Fine timing refinement (Bug #39).
	// The coarse search at symbol-period steps can be off by up to ±Nofdm/2
	// base-rate samples from the true ACK start. For NB (Ngi=16), the max
	// error of ±136 samples far exceeds the GI tolerance → ICI → low metric.
	// Search at base-rate (IR-step) resolution within ±Nofdm/2 of the coarse
	// position. Only runs when Phase 1 found a decent candidate.
	if (best_matched >= 6 && best_pos >= 0)
	{
		int coarse_offset = best_pos * sym_period_interp;
		int search_half = sym_period_interp / 2;
		double fine_best_metric = -1.0;
		int fine_best_matched = 0;
		int fine_best_offset = coarse_offset;

		for (int d = coarse_offset - search_half; d <= coarse_offset + search_half;
		     d += interpolation_rate)
		{
			if (d < 0) continue;

			double metric_f = 0;
			int matched_f = 0;
			bool oob = false;

			for (int p = 0; p < ack_nsymb && !oob; p++)
			{
				int offset = d + p * sym_period_interp + Ngi * interpolation_rate;
				if (offset + Nfft * interpolation_rate > buffer_size_interp)
				{
					oob = true;
					break;
				}

				for (int i = 0; i < Nfft; i++)
					decimated_sym[i] = baseband_interp[offset + i * interpolation_rate];
				fft(decimated_sym, fft_out, Nfft);

				int tone_base = ack_tones[p % ack_pattern_len];
				int actual_tone = (tone_base + p * tone_hop_step) % mfsk_M;

				int streams_ok = 0;
				double e_targ = 0;
				for (int st = 0; st < nStreams; st++)
				{
					int esub = stream_offsets[st] + actual_tone;
					int ebin = (esub < half) ? Nfft - half + esub
					                         : start_shift + (esub - half);
					double ee = fft_out[ebin].real() * fft_out[ebin].real() +
					            fft_out[ebin].imag() * fft_out[ebin].imag();
					int mbin = (Nfft - ebin) % Nfft;
					double em = fft_out[mbin].real() * fft_out[mbin].real() +
					            fft_out[mbin].imag() * fft_out[mbin].imag();
					e_targ += ee + em;

					double pk = -1.0;
					int pkbin = -1;
					for (int t = 0; t < mfsk_M; t++)
					{
						int sub = stream_offsets[st] + t;
						int b = (sub < half) ? Nfft - half + sub
						                     : start_shift + (sub - half);
						double e = fft_out[b].real() * fft_out[b].real() +
						           fft_out[b].imag() * fft_out[b].imag();
						if (e > pk) { pk = e; pkbin = b; }
					}
					if (pk > 0 && (pkbin == ebin || pkbin == mbin))
						streams_ok++;
				}

				if (streams_ok < nStreams) continue;
				matched_f++;

				double e_tot = 0;
				for (int k = 0; k < Nc; k++)
				{
					int bk = (k < half) ? Nfft - half + k
					                    : start_shift + (k - half);
					double e = fft_out[bk].real() * fft_out[bk].real() +
					           fft_out[bk].imag() * fft_out[bk].imag();
					e_tot += e;
				}
				if (e_tot > 0)
					metric_f += e_targ / e_tot;
			}

			if (oob) continue;
			if (matched_f > fine_best_matched ||
			    (matched_f == fine_best_matched && metric_f > fine_best_metric))
			{
				fine_best_matched = matched_f;
				fine_best_metric = metric_f;
				fine_best_offset = d;
			}
		}

		// Use fine result if better than coarse
		if (fine_best_matched > best_matched ||
		    (fine_best_matched == best_matched && fine_best_metric > best_metric))
		{
			best_matched = fine_best_matched;
			best_metric = fine_best_metric;
			best_pos = fine_best_offset / sym_period_interp;
		}
	}

	if (out_matched)
		*out_matched = best_matched;

	return best_metric;
}

int cl_ofdm::symbol_sync(std::complex <double>*in, int size, int interpolation_rate, int location_to_return)
{

	double corss_corr=0;
	double norm_a=0;
	double norm_b=0;

	int *corss_corr_loc=new int[Nsymb];
	double *corss_corr_vals=new double[Nsymb];
	int return_val;

	std::complex <double> *a_c, *b_c, a, b;

	for(int i=0;i<Nsymb;i++)
	{
		corss_corr_loc[i]=-1;
		corss_corr_vals[i]=0;
	}

	for(int i=0;i<Nsymb;i++)
	{
		a_c=in+i*(Nfft+Ngi)*interpolation_rate;
		b_c=in+i*(Nfft+Ngi)*interpolation_rate+(Nfft/2)*interpolation_rate;
		corss_corr=0;
		norm_a=0;
		norm_b=0;
		for(int m=0;m<(this->Nfft/2)*interpolation_rate;m++)
		{
			corss_corr+=a_c[m].real()*b_c[m].real();
			norm_a+=a_c[m].real()*a_c[m].real();
			norm_b+=b_c[m].real()*b_c[m].real();

			corss_corr+=a_c[m].imag()*b_c[m].imag();
			norm_a+=a_c[m].imag()*a_c[m].imag();
			norm_b+=b_c[m].imag()*b_c[m].imag();
		}
		corss_corr=corss_corr/sqrt(norm_a*norm_b);

		if(corss_corr<0)
		{
			corss_corr_vals[i]=-corss_corr;
		}
		else
		{
			corss_corr_vals[i]=corss_corr;
		}
		corss_corr_loc[i]=i;

	}
	double tmp;
	int tmp_int;
	for(int i=0;i<Nsymb-1;i++)
	{
		for(int j=0;j<Nsymb-1;j++)
		{
			if (corss_corr_vals[j]<corss_corr_vals[j+1])
			{
				tmp=corss_corr_vals[j];
				corss_corr_vals[j]=corss_corr_vals[j+1];
				corss_corr_vals[j+1]=tmp;

				tmp_int=corss_corr_loc[j];
				corss_corr_loc[j]=corss_corr_loc[j+1];
				corss_corr_loc[j+1]=tmp_int;
			}
		}
	}
	return_val=corss_corr_loc[location_to_return];
	if(corss_corr_loc!=NULL)
	{
		delete[] corss_corr_loc;
	}
	if(corss_corr_vals!=NULL)
	{
		delete[] corss_corr_vals;
	}
	return return_val;
}

void cl_ofdm::rational_resampler(std::complex <double>* in, int in_size, std::complex <double>* out, int rate, int interpolation_decimation)
{
	if (interpolation_decimation==DECIMATION)
	{
		int index=0;
		for(int i=0;i<in_size;i+=rate)
		{
			*(out+index)=*(in+i);
			index++;
		}
	}
	else if (interpolation_decimation==INTERPOLATION)
	{
		for(int i=0;i<in_size-1;i++)
		{
			for(int j=0;j<rate;j++)
			{
				*(out+i*rate+j)=interpolate_linear(*(in+i),0,*(in+i+1),rate,j);
			}
		}
		for(int j=0;j<rate;j++)
		{
			*(out+(in_size-1)*rate+j)=interpolate_linear(*(in+in_size-2),0,*(in+in_size-1),rate,rate+j);
		}
	}
}

void cl_ofdm::baseband_to_passband(std::complex <double>* in, int in_size, double* out, double sampling_frequency, double carrier_frequency, double carrier_amplitude,int interpolation_rate)
{
	double sampling_interval=1.0/sampling_frequency;

	// Grow-as-needed interpolation buffer
	int needed = in_size * interpolation_rate;
	if(needed > b2p_buffer_size)
	{
		if(b2p_data_interpolated!=NULL) delete[] b2p_data_interpolated;
		b2p_data_interpolated = new std::complex<double>[needed];
		b2p_buffer_size = needed;
	}
	std::complex <double> *data_interpolated = b2p_data_interpolated;

	rational_resampler( in, in_size, data_interpolated, interpolation_rate, INTERPOLATION);
	for(int i=0;i<in_size*interpolation_rate;i++)
	{
		out[i]=data_interpolated[i].real()*carrier_amplitude*cos(2*M_PI*carrier_frequency*(double)passband_start_sample * sampling_interval);
		out[i]+=data_interpolated[i].imag()*carrier_amplitude*sin(2*M_PI*carrier_frequency*(double)passband_start_sample * sampling_interval);
		passband_start_sample++;
	}
}
void cl_ofdm::passband_to_baseband(double* in, int in_size, std::complex <double>* out, double sampling_frequency, double carrier_frequency, double carrier_amplitude, int decimation_rate, cl_FIR* filter, int sample_offset)
{
	double sampling_interval=1.0/sampling_frequency;

	// Reuse pre-allocated buffers (reallocate only if size changed)
	if(p2b_buffer_size < in_size)
	{
		delete[] p2b_l_data;
		delete[] p2b_data_filtered;
		p2b_l_data = new std::complex<double>[in_size];
		p2b_data_filtered = new std::complex<double>[in_size];
		p2b_buffer_size = in_size;
	}

	for(int i=0;i<in_size;i++)
	{
		p2b_l_data[i].real(in[i]*carrier_amplitude*cos(2*M_PI*carrier_frequency*(double)(i+sample_offset) * sampling_interval));
		p2b_l_data[i].imag(in[i]*carrier_amplitude*sin(2*M_PI*carrier_frequency*(double)(i+sample_offset) * sampling_interval));
	}

	filter->apply(p2b_l_data,p2b_data_filtered,in_size);

	rational_resampler(p2b_data_filtered, in_size, out, decimation_rate, DECIMATION);
}
