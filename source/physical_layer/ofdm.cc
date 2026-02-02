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
}

cl_ofdm::~cl_ofdm()
{
	this->deinit();
}

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

	ofdm_frame = new struct st_carrier[this->Nsymb*this->Nc];
	zero_padded_data=new std::complex <double>[Nfft];
	iffted_data=new std::complex <double>[Nfft];
	gi_removed_data=new std::complex <double>[Nfft];
	ffted_data=new std::complex <double>[Nfft];
	estimated_channel=new struct st_channel_complex[this->Nsymb*this->Nc];
	estimated_channel_without_amplitude_restoration=new struct st_channel_complex[this->Nsymb*this->Nc];
	ofdm_preamble = new struct st_carrier[this->preamble_configurator.Nsymb*this->Nc];
	passband_start_sample=0;

	preamble_configurator.init(this->Nfft, this->Nc,this->ofdm_preamble, this->start_shift);
	pilot_configurator.init(this->Nfft, this->Nc,this->Nsymb,this->ofdm_frame, this->start_shift);


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


	if(ofdm_frame!=NULL)
	{
		delete[] ofdm_frame;
		ofdm_frame=NULL;
	}

	if(ofdm_preamble!=NULL)
	{
		delete[] ofdm_preamble;
		ofdm_preamble=NULL;
	}
	if(zero_padded_data!=NULL)
	{
		delete[] zero_padded_data;
		zero_padded_data=NULL;
	}
	if(iffted_data!=NULL)
	{
		delete[] iffted_data;
		iffted_data=NULL;
	}
	if(gi_removed_data!=NULL)
	{
		delete[] gi_removed_data;
		gi_removed_data=NULL;
	}
	if(ffted_data!=NULL)
	{
		delete[] ffted_data;
		ffted_data=NULL;
	}
	if(estimated_channel!=NULL)
	{
		delete[] estimated_channel;
		estimated_channel=NULL;
	}

	if(estimated_channel_without_amplitude_restoration!=NULL)
	{
		delete[] estimated_channel_without_amplitude_restoration;
		estimated_channel_without_amplitude_restoration=NULL;
	}

	pilot_configurator.deinit();
	preamble_configurator.deinit();
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
	_fft(out,Nfft);

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
	_fft(out,_Nfft);

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
	_ifft(out,Nfft);
}

void cl_ofdm::ifft(std::complex <double>* in, std::complex <double>* out,int _Nfft)
{
	for(int i=0;i<_Nfft;i++)
	{
		out[i]=in[i];
	}
	_ifft(out,_Nfft);
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
		printf("[COARSE-FREQ] Low energy (%.3f < %.1f) - skip\n", input_energy, min_energy);
		fflush(stdout);
		return 0.0;  // No signal, don't apply any correction
	}

	printf("[COARSE-FREQ] Entry: Nfft=%d Ngi=%d interp=%d energy=%.3f\n",
		   Nfft, Ngi, interpolation_rate, input_energy);
	fflush(stdout);

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
	printf("[COARSE-FREQ] Fractional CFO: %.4f subcarriers (|P|=%.6f R=%.6f corr_mag=%.4f)\n",
	       frac_cfo_subcarriers, std::abs(P), R, corr_mag);

	// Gate on correlation quality - low correlation means we're not looking at a valid preamble
	// A good preamble detection should have corr_mag > 0.5
	const double min_corr_mag = 0.5;
	if (corr_mag < min_corr_mag) {
		printf("[COARSE-FREQ] Low correlation (%.3f < %.1f) - skip\n", corr_mag, min_corr_mag);
		fflush(stdout);
		return 0.0;
	}
	fflush(stdout);

	// Note: We don't early exit here because the fractional CFO from noise
	// is typically close to 0 anyway, and we'll check confidence on the
	// integer CFO estimate before applying any large corrections.

	// Step 2: Integer CFO estimation
	// Apply fractional correction and FFT the preamble symbol
	// Note: Input is at interpolation_rate, so we downsample by taking every
	// interpolation_rate-th sample to get back to baseband (Nfft samples)
	std::complex<double>* corrected_symbol = new std::complex<double>[Nfft];
	std::complex<double>* fft_out = new std::complex<double>[Nfft];

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

	// Debug: check FFT energy
	double total_fft_energy = 0.0;
	double max_bin_energy = 0.0;
	int max_bin = 0;
	for (int i = 0; i < Nfft; i++) {
		double e = std::norm(fft_out[i]);
		total_fft_energy += e;
		if (e > max_bin_energy) {
			max_bin_energy = e;
			max_bin = i;
		}
	}
	printf("[COARSE-FREQ] FFT: total_energy=%.4f max_bin=%d max_energy=%.4f Nfft=%d Nc=%d\n",
		   total_fft_energy, max_bin, max_bin_energy, Nfft, Nc);
	fflush(stdout);

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
		printf("[COARSE-FREQ] Integer CFO: best k=%d metric=%.4f\n", best_int_cfo, best_metric);
		fflush(stdout);
	}

	// Cleanup
	delete[] corrected_symbol;
	delete[] fft_out;

	// Determine effective integer CFO (only use if confident)
	int effective_int_cfo = 0;
	if (search_limit > 0 && best_metric > 2.0)
	{
		effective_int_cfo = best_int_cfo;
	}

	// Total CFO: fractional + integer (if confident)
	double total_cfo_subcarriers = frac_cfo_subcarriers + (double)effective_int_cfo;
	double total_cfo_hz = total_cfo_subcarriers * subcarrier_spacing;

	printf("[COARSE-FREQ] Result: frac=%.3f int=%d total=%.1f Hz (corr=%.2f)\n",
	       frac_cfo_subcarriers, effective_int_cfo, total_cfo_hz, corr_mag);
	fflush(stdout);

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
	if(virtual_carrier!=NULL)
	{
		delete[] virtual_carrier;
	}
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
	virtual_carrier = new struct st_carrier[this->Nc_max*this->Nc_max];

	for(int j=0;j<this->Nc_max;j++)
	{
		for(int i=0;i<this->Nc_max;i++)
		{
			(virtual_carrier+j*this->Nc_max+i)->type=DATA;
		}

	}

	this->configure();

	sequence = new std::complex <double>[nPilots];

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

	if(virtual_carrier!=NULL)
	{
		delete[] virtual_carrier;
		virtual_carrier=NULL;
	}
	if(sequence!=NULL)
	{
		delete[] sequence;
		sequence=NULL;
	}

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

	sequence = new std::complex <double>[this->Nsymb*this->Nc];

	if(print_on==YES)
	{
		this->print();
	}

	__srandom(seed);
	for(int i=0;i<this->Nsymb*this->Nc;i++)
	{
		if(modulation==MOD_BPSK)
		{
			sequence[i]=std::complex <double>(2*(__random()%2)-1,0);
		}
		else if(modulation==MOD_QPSK)
		{
			sequence[i]=std::complex <double>(2*(__random()%2)-1,2*(__random()%2)-1)/sqrt(2);
		}
	}

	int preamble_index=0;
	for(int j=0;j<this->Nsymb;j++)
	{
		for(int i=0;i<this->Nc;i++)
		{
			if ((carrier+j*this->Nc+i)->type==ZERO)
			{
				(carrier+j*this->Nc+i)->value=0;
			}
			else if ((carrier+j*this->Nc+i)->type==PREAMBLE)
			{
				(carrier+j*this->Nc+i)->value=sequence[preamble_index];
				preamble_index++;
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

	if(sequence!=NULL)
	{
		delete[] sequence;
		sequence=NULL;
	}

}

void cl_preamble_configurator::configure()
{

	int fft_zeros_tmp[Nfft];
	int fft_zeros_depadded_tmp[Nc];

	for(int j=0;j<Nfft;j++)
	{
		if(j%2==1)
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

	nZeros *= Nsymb;
	nPreamble *= Nsymb;

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

	int *corss_corr_loc=new int[size];
	double *corss_corr_vals=new double[size];
	int return_val;


	std::complex <double> *a_c, *b_c, *data;

	for(int i=0;i<size;i++)
	{
		corss_corr_loc[i]=-1;
		corss_corr_vals[i]=0;
	}

	data= new std::complex <double> [preamble_configurator.Nsymb*(this->Ngi+this->Nfft)*interpolation_rate];

	for(int i=0;i<size-preamble_configurator.Nsymb*(this->Ngi+this->Nfft)*interpolation_rate;i+=step)
	{
		for(int k=0;k<preamble_configurator.Nsymb*(this->Ngi+this->Nfft)*interpolation_rate;k++)
		{
			data[k]=*(in+i+k);
		}

		corss_corr=0;
		norm_a=0;
		norm_b=0;
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

		corss_corr=corss_corr/sqrt(norm_a*norm_b);
		corss_corr_vals[i]=corss_corr;
		corss_corr_loc[i]=i;
	}

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
	if(corss_corr_loc!=NULL)
	{
		delete[] corss_corr_loc;
	}
	if(corss_corr_vals!=NULL)
	{
		delete[] corss_corr_vals;
	}
	if(data!=NULL)
	{
		delete[] data;
	}
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

	int *corss_corr_loc=new int[size];
	double *corss_corr_vals=new double[size];
	TimeSyncResult result;
	result.delay = 0;
	result.correlation = 0.0;

	std::complex <double> *a_c, *b_c, *data;

	for(int i=0;i<size;i++)
	{
		corss_corr_loc[i]=-1;
		corss_corr_vals[i]=0;
	}

	data= new std::complex <double> [preamble_configurator.Nsymb*(this->Ngi+this->Nfft)*interpolation_rate];

	for(int i=0;i<size-preamble_configurator.Nsymb*(this->Ngi+this->Nfft)*interpolation_rate;i+=step)
	{
		for(int k=0;k<preamble_configurator.Nsymb*(this->Ngi+this->Nfft)*interpolation_rate;k++)
		{
			data[k]=*(in+i+k);
		}

		corss_corr=0;
		norm_a=0;
		norm_b=0;
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

		corss_corr=corss_corr/sqrt(norm_a*norm_b);
		corss_corr_vals[i]=corss_corr;
		corss_corr_loc[i]=i;
	}

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

	if(corss_corr_loc!=NULL)
	{
		delete[] corss_corr_loc;
	}
	if(corss_corr_vals!=NULL)
	{
		delete[] corss_corr_vals;
	}
	if(data!=NULL)
	{
		delete[] data;
	}
	return result;
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
	std::complex <double> *data_interpolated= new std::complex <double>[in_size*interpolation_rate];
	rational_resampler( in, in_size, data_interpolated, interpolation_rate, INTERPOLATION);
	for(int i=0;i<in_size*interpolation_rate;i++)
	{
		out[i]=data_interpolated[i].real()*carrier_amplitude*cos(2*M_PI*carrier_frequency*(double)passband_start_sample * sampling_interval);
		out[i]+=data_interpolated[i].imag()*carrier_amplitude*sin(2*M_PI*carrier_frequency*(double)passband_start_sample * sampling_interval);
		passband_start_sample++;
	}
	if(data_interpolated!=NULL)
	{
		delete[] data_interpolated;
	}
}
void cl_ofdm::passband_to_baseband(double* in, int in_size, std::complex <double>* out, double sampling_frequency, double carrier_frequency, double carrier_amplitude, int decimation_rate, cl_FIR* filter)
{
	double sampling_interval=1.0/sampling_frequency;

	std::complex <double> *l_data= new std::complex <double>[in_size];
	std::complex <double> *data_filtered= new std::complex <double>[in_size];

	for(int i=0;i<in_size;i++)
	{
		l_data[i].real(in[i]*carrier_amplitude*cos(2*M_PI*carrier_frequency*(double)i * sampling_interval));
		l_data[i].imag(in[i]*carrier_amplitude*sin(2*M_PI*carrier_frequency*(double)i * sampling_interval));
	}

	filter->apply(l_data,data_filtered,in_size);

	rational_resampler(data_filtered, in_size, out, decimation_rate, DECIMATION);
	if(l_data!=NULL)
	{
		delete[] l_data;
	}
	if(data_filtered!=NULL)
	{
		delete[] data_filtered;
	}
}
