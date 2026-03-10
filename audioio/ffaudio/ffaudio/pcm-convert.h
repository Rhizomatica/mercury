/** ffaudio: PCM conversion.
2015, Simon Zolin */

#include <ffaudio/audio.h>
#include <ffaudio/pcm.h>
#include <ffbase/base.h>

enum CHAN_MASK {
	CHAN_FL = 1,
	CHAN_FR = 2,
	CHAN_FC = 4,
	CHAN_LFE = 8,
	CHAN_BL = 0x10,
	CHAN_BR = 0x20,
	CHAN_SL = 0x40,
	CHAN_SR = 0x80,
};

/** Get channel mask by channels number. */
static uint chan_mask(uint channels)
{
	switch (channels) {
	case 1:
		return CHAN_FC;
	case 2:
		return CHAN_FL | CHAN_FR;
	case 6:
		return CHAN_FL | CHAN_FR | CHAN_FC | CHAN_LFE | CHAN_BL | CHAN_BR;
	case 8:
		return CHAN_FL | CHAN_FR | CHAN_FC | CHAN_LFE | CHAN_BL | CHAN_BR | CHAN_SL | CHAN_SR;
	}
	return -1;
}

#define BIT32(bit)  (1U << (bit))

/** Set gain level for all used channels. */
static int chan_fill_gain_levels(double level[8][8], uint imask, uint omask)
{
	enum {
		FL,
		FR,
		FC,
		LFE,
		BL,
		BR,
		SL,
		SR,
	};

	const double sqrt1_2 = 0.70710678118654752440; // =1/sqrt(2)

	uint equal = imask & omask;
	for (uint c = 0;  c != 8;  c++) {
		if (equal & BIT32(c))
			level[c][c] = 1;
	}

	uint unused = imask & ~omask;

	if (unused & CHAN_FL) {

		if (omask & CHAN_FC) {
			// front stereo -> front center
			level[FC][FL] = sqrt1_2;
			level[FC][FR] = sqrt1_2;

		} else
			return -1;
	}

	if (unused & CHAN_FC) {

		if (omask & CHAN_FL) {
			// front center -> front stereo
			level[FL][FC] = sqrt1_2;
			level[FR][FC] = sqrt1_2;

		} else
			return -1;
	}

	if (unused & CHAN_LFE) {
	}

	if (unused & CHAN_BL) {

		if (omask & CHAN_FL) {
			// back stereo -> front stereo
			level[FL][BL] = sqrt1_2;
			level[FR][BR] = sqrt1_2;

		} else if (omask & CHAN_FC) {
			// back stereo -> front center
			level[FC][BL] = sqrt1_2*sqrt1_2;
			level[FC][BR] = sqrt1_2*sqrt1_2;

		} else
			return -1;
	}

	if (unused & CHAN_SL) {

		if (omask & CHAN_FL) {
			// side stereo -> front stereo
			level[FL][SL] = sqrt1_2;
			level[FR][SR] = sqrt1_2;

		} else if (omask & CHAN_FC) {
			// side stereo -> front center
			level[FC][SL] = sqrt1_2*sqrt1_2;
			level[FC][SR] = sqrt1_2*sqrt1_2;

		} else
			return -1;
	}

	// now gain level can be >1.0, so we normalize it
	for (uint oc = 0;  oc != 8;  oc++) {
		if (!ffbit_test32(&omask, oc))
			continue;

		double sum = 0;
		for (uint ic = 0;  ic != 8;  ic++) {
			sum += level[oc][ic];
		}
		if (sum != 0) {
			for (uint ic = 0;  ic != 8;  ic++) {
				level[oc][ic] /= sum;
			}
		}
	}

	return 0;
}

/** Mix (upmix, downmix) channels.
ochan: Output channels number
odata: Output data; float, interleaved

Supported layouts:
1: FC
2: FL+FR
5.1: FL+FR+FC+LFE+BL+BR
7.1: FL+FR+FC+LFE+BL+BR+SL+SR

Examples:

5.1 -> 1:
	FC = FL*0.7 + FR*0.7 + FC*1 + BL*0.5 + BR*0.5

5.1 -> 2:
	FL = FL*1 + FC*0.7 + BL*0.7
	FR = FR*1 + FC*0.7 + BR*0.7
*/
static int _pcm_chan_mix(uint ochan, void *odata, const struct pcm_af *inpcm, const void *idata, size_t samples)
{
	union pcm_data in, out;
	double level[8][8] = {}; // gain level [OUT] <- [IN]
	void *ini[8];
	uint istep, ostep; // intervals between samples of the same channel
	uint ic, oc, ocstm; // channel counters
	uint imask, omask; // channel masks
	size_t i;

	imask = chan_mask(inpcm->channels);
	omask = chan_mask(ochan);
	if (imask == 0 || omask == 0)
		return -1;

	if (0 != chan_fill_gain_levels(level, imask, omask))
		return -1;

	// set non-interleaved input array
	istep = 1;
	in.pi8 = (char**)idata;
	if (inpcm->interleaved) {
		pcm_setni(ini, (void*)idata, inpcm->format, inpcm->channels);
		in.pi8 = (char**)ini;
		istep = inpcm->channels;
	}

	// set interleaved output array
	out.f32 = (float*)odata;
	ostep = ochan;

	ocstm = 0;
	switch (inpcm->format) {
	case FFAUDIO_F_INT16:
		for (oc = 0;  oc != 8;  oc++) {

			if (!ffbit_test32(&omask, oc))
				continue;

			for (i = 0;  i != samples;  i++) {
				double sum = 0;
				uint icstm = 0;
				for (ic = 0;  ic != 8;  ic++) {
					if (!ffbit_test32(&imask, ic))
						continue;
					sum += pcm_flt_i16(in.pi16[icstm][i * istep]) * level[oc][ic];
					icstm++;
				}
				out.f32[ocstm + i * ostep] = pcm_limf(sum);
			}

			if (++ocstm == ochan)
				break;
		}
		break;

	case FFAUDIO_F_INT24:
		for (uint oc = 0;  oc != 8;  oc++) {
			if (ffbit_test32(&omask, oc)) {
				for (uint i = 0;  i != samples;  i++) {
					double sum = 0;
					uint icstm = 0;
					for (uint ic = 0;  ic != 8;  ic++) {
						if (ffbit_test32(&imask, ic)) {
							sum += pcm_flt_i24(pcm_i32_i24(&in.pi8[icstm++][i * istep * 3])) * level[oc][ic];
						}
					}
					out.f32[ocstm + i * ostep] = pcm_limf(sum);
				}
			}

			if (++ocstm == ochan)
				break;
		}
		break;

	case FFAUDIO_F_INT32:
		for (oc = 0;  oc != 8;  oc++) {

			if (!ffbit_test32(&omask, oc))
				continue;

			for (i = 0;  i != samples;  i++) {
				double sum = 0;
				uint icstm = 0;
				for (ic = 0;  ic != 8;  ic++) {
					if (!ffbit_test32(&imask, ic))
						continue;
					sum += pcm_flt_i32(in.pi32[icstm][i * istep]) * level[oc][ic];
					icstm++;
				}
				out.f32[ocstm + i * ostep] = pcm_limf(sum);
			}

			if (++ocstm == ochan)
				break;
		}
		break;

	case FFAUDIO_F_FLOAT32:
		for (oc = 0;  oc != 8;  oc++) {

			if (!ffbit_test32(&omask, oc))
				continue;

			for (i = 0;  i != samples;  i++) {
				double sum = 0;
				uint icstm = 0;
				for (ic = 0;  ic != 8;  ic++) {
					if (!ffbit_test32(&imask, ic))
						continue;
					sum += in.pf32[icstm][i * istep] * level[oc][ic];
					icstm++;
				}
				out.f32[ocstm + i * ostep] = pcm_limf(sum);
			}

			if (++ocstm == ochan)
				break;
		}
		break;

	case FFAUDIO_F_FLOAT64:
		for (oc = 0;  oc != 8;  oc++) {

			if (!ffbit_test32(&omask, oc))
				continue;

			for (i = 0;  i != samples;  i++) {
				double sum = 0;
				uint icstm = 0;
				for (ic = 0;  ic != 8;  ic++) {
					if (!ffbit_test32(&imask, ic))
						continue;
					sum += in.pf64[icstm][i * istep] * level[oc][ic];
					icstm++;
				}
				out.f64[ocstm + i * ostep] = pcm_limf(sum);
			}

			if (++ocstm == ochan)
				break;
		}
		break;

	default:
		return -1;
	}

	return 0;
}

#define X(f1, f2) \
	(f1 << 16) | (f2 & 0xffff)

// IFFF IFFF
#define X4(f1, i1, f2, i2) \
	(i1 << 31) | (f1 << 16) | (i2 << 15) | (f2 & 0xfff)

/* Priority formats:
Library     Format
======================
libfdk-aac  int16/i
CD WAV      int16/i
CD libFLAC  int16/ni
HD libFLAC  int24/ni
libsox      int32/i
libopus     float32/i
libmpg123   float32/i
libvorbis   float32/ni
DANorm      float64/ni
*/

static int _pcm_convert_stereo(unsigned format_hash, void *out, const void *in, unsigned in_ileaved, size_t samples)
{
	// Priority output formats:
	// int16/i
	// int32/i
	// float32/i

	size_t i;
	union pcm_data o, iL, iR = {};
	o.p = out;
	iL.p = (void*)in;
	if (!in_ileaved && in) {
		iL.p = ((void**)in)[0];
		iR.p = ((void**)in)[1];
	}

	switch (format_hash) {

	// int <- int:

	case X4(FFAUDIO_F_INT16, 1, FFAUDIO_F_INT16, 0):
		for (i = 0;  i < samples;  i++) {
			*o.i16++ = iL.i16[i];
			*o.i16++ = iR.i16[i];
		}
		break;

	case X4(FFAUDIO_F_INT16, 1, FFAUDIO_F_INT24, 0):
		for (i = 0;  i < samples;  i++) {
			*o.i16++ = (int)ffint_le_cpu24_ptr(iL.i8 + i * 3) / 0x100;
			*o.i16++ = (int)ffint_le_cpu24_ptr(iR.i8 + i * 3) / 0x100;
		}
		break;

	case X4(FFAUDIO_F_INT16, 1, FFAUDIO_F_INT32, 1):
		for (i = 0;  i < samples;  i++) {
			*o.i16++ = *iL.i32++;
			*o.i16++ = *iL.i32++;
		}
		break;

	case X4(FFAUDIO_F_INT32, 1, FFAUDIO_F_INT16, 0):
		for (i = 0;  i < samples;  i++) {
			*o.i32++ = (int)iL.i16[i] * 0x10000;
			*o.i32++ = (int)iR.i16[i] * 0x10000;
		}
		break;

	case X4(FFAUDIO_F_INT32, 1, FFAUDIO_F_INT16, 1):
		for (i = 0;  i < samples;  i++) {
			*o.i32++ = (int)*iL.i16++ * 0x10000;
			*o.i32++ = (int)*iL.i16++ * 0x10000;
		}
		break;

	case X4(FFAUDIO_F_INT32, 1, FFAUDIO_F_INT24, 0):
		for (i = 0;  i < samples;  i++) {
			*o.i32++ = (int)ffint_le_cpu24_ptr(iL.i8 + i * 3) * 0x100;
			*o.i32++ = (int)ffint_le_cpu24_ptr(iR.i8 + i * 3) * 0x100;
		}
		break;

	// int <- float:

	case X4(FFAUDIO_F_INT16, 1, FFAUDIO_F_FLOAT32, 0):
		for (i = 0;  i < samples;  i++) {
			*o.i16++ = pcm_i16_flt(iL.f32[i]);
			*o.i16++ = pcm_i16_flt(iR.f32[i]);
		}
		break;

	case X4(FFAUDIO_F_INT16, 1, FFAUDIO_F_FLOAT32, 1):
		for (i = 0;  i < samples;  i++) {
			*o.i16++ = pcm_i16_flt(*iL.f32++);
			*o.i16++ = pcm_i16_flt(*iL.f32++);
		}
		break;

	case X4(FFAUDIO_F_INT24, 1, FFAUDIO_F_FLOAT32, 1):
		for (i = 0;  i < samples;  i++) {
			pcm_i24_i32(o.i8, pcm_i24_flt(*iL.f32++));
			o.i8 += 3;
			pcm_i24_i32(o.i8, pcm_i24_flt(*iL.f32++));
			o.i8 += 3;
		}
		break;

	case X4(FFAUDIO_F_INT32, 1, FFAUDIO_F_FLOAT32, 0):
		for (i = 0;  i < samples;  i++) {
			*o.i32++ = pcm_i32_flt(iL.f32[i]);
			*o.i32++ = pcm_i32_flt(iR.f32[i]);
		}
		break;

	case X4(FFAUDIO_F_INT32, 1, FFAUDIO_F_FLOAT32, 1):
		for (i = 0;  i < samples;  i++) {
			*o.i32++ = pcm_i32_flt(*iL.f32++);
			*o.i32++ = pcm_i32_flt(*iL.f32++);
		}
		break;

	// float <- int:

	case X4(FFAUDIO_F_FLOAT32, 1, FFAUDIO_F_INT16, 0):
		for (i = 0;  i < samples;  i++) {
			*o.f32++ = pcm_flt_i16(iL.i16[i]);
			*o.f32++ = pcm_flt_i16(iR.i16[i]);
		}
		break;

	case X4(FFAUDIO_F_FLOAT32, 1, FFAUDIO_F_INT16, 1):
		for (i = 0;  i < samples;  i++) {
			*o.f32++ = pcm_flt_i16(*iL.i16++);
			*o.f32++ = pcm_flt_i16(*iL.i16++);
		}
		break;

	case X4(FFAUDIO_F_FLOAT32, 1, FFAUDIO_F_INT24, 0):
		for (i = 0;  i < samples;  i++) {
			*o.f32++ = pcm_flt_i24(pcm_i32_i24(iL.i8 + i * 3));
			*o.f32++ = pcm_flt_i24(pcm_i32_i24(iR.i8 + i * 3));
		}
		break;

	case X4(FFAUDIO_F_FLOAT32, 1, FFAUDIO_F_INT32, 1):
		for (i = 0;  i < samples;  i++) {
			*o.f32++ = pcm_flt_i32(*iL.i32++);
			*o.f32++ = pcm_flt_i32(*iL.i32++);
		}
		break;

	default:
		return 1;
	}

	return 0;
}

/** Convert PCM samples
Note: sample rate conversion isn't supported. */
/* Algorithm:
If channels don't match, do channel conversion:
  . upmix/downmix: mix appropriate channels with each other.  Requires additional memory buffer.
  . mono: copy data for 1 channel only, skip other channels

If format and "interleaved" flags match for both input and output, just copy the data.
Otherwise, process each channel and sample in a loop.

non-interleaved: data[0][..] - left,  data[1][..] - right
interleaved: data[0,2..] - left */
static inline int pcm_convert(const struct pcm_af *outpcm, void *out, const struct pcm_af *inpcm, const void *in, size_t samples)
{
	size_t i;
	uint ich, nch = inpcm->channels, in_ileaved = inpcm->interleaved;
	union pcm_data from, to;
	void *tmpptr = NULL;
	int r = -1;
	void *ini[PCM_CHAN_MAX], *oni[PCM_CHAN_MAX];
	uint istep = 1, ostep = 1;
	uint ifmt;

	from.p = (void*)in;
	ifmt = inpcm->format;

	to.p = out;

	if (inpcm->channels > PCM_CHAN_MAX || (outpcm->channels & PCM_CHAN_MASK) > PCM_CHAN_MAX)
		goto done;

	if (inpcm->rate != outpcm->rate)
		goto done;

	if (inpcm->channels != outpcm->channels) {

		nch = outpcm->channels & PCM_CHAN_MASK;

		if (nch == 1 && (outpcm->channels & ~PCM_CHAN_MASK) != 0) {
			uint ch = ((outpcm->channels & ~PCM_CHAN_MASK) >> 4) - 1;
			if (ch > 1)
				goto done;

			if (!inpcm->interleaved) {
				from.pi16 = from.pi16 + ch;

			} else {
				ini[0] = from.i8 + ch * pcm_f_bits(inpcm->format) / 8;
				from.pi8 = (char**)ini;
				istep = inpcm->channels;
				in_ileaved = 0;
			}

		} else if ((outpcm->channels & ~PCM_CHAN_MASK) == 0) {
			if (NULL == (tmpptr = ffmem_alloc(samples * nch * sizeof(float))))
				goto done;

			if (0 != _pcm_chan_mix(nch, tmpptr, inpcm, in, samples))
				goto done;

			if (outpcm->interleaved) {
				from.i8 = (char*)tmpptr;
				in_ileaved = 1;

			} else {
				pcm_setni(ini, tmpptr, FFAUDIO_F_FLOAT32, nch);
				from.pi8 = (char**)ini;
				istep = nch;
				in_ileaved = 0;
			}
			ifmt = FFAUDIO_F_FLOAT32;

		} else {
			goto done; // this channel conversion is not supported
		}
	}

	if (ifmt == outpcm->format && istep == 1) {
		// input & output formats are the same, try to copy data directly

		if (in_ileaved != outpcm->interleaved && nch == 1) {
			if (samples == 0)
			{}
			else if (!in_ileaved) {
				// non-interleaved input mono -> interleaved input mono
				from.i8 = from.pi8[0];
			} else {
				// interleaved input mono -> non-interleaved input mono
				ini[0] = from.i8;
				from.pi8 = (char**)ini;
			}
			in_ileaved = outpcm->interleaved;
		}

		if (in_ileaved == outpcm->interleaved) {
			if (samples == 0)
				;
			else if (in_ileaved) {
				// interleaved input -> interleaved output
				memcpy(to.i8, from.i8, samples * pcm_f_bits(ifmt)/8 * nch);
			} else {
				// non-interleaved input -> non-interleaved output
				for (ich = 0;  ich != nch;  ich++) {
					memcpy(to.pi8[ich], from.pi8[ich], samples * pcm_f_bits(ifmt)/8);
				}
			}
			r = 0;
			goto done;
		}
	}

	r = 0;
	if (nch == 2) {
		if (!_pcm_convert_stereo(X4(outpcm->format, outpcm->interleaved, ifmt, in_ileaved), to.i8, from.i8, in_ileaved, samples))
			goto done;
	}

	if (in_ileaved) {
		from.pi8 = pcm_setni(ini, from.i8, ifmt, nch);
		istep = nch;
	}

	if (outpcm->interleaved) {
		to.pi8 = pcm_setni(oni, to.i8, outpcm->format, nch);
		ostep = nch;
	}

	switch (X(ifmt, outpcm->format)) {

// uint8
	case X(FFAUDIO_F_UINT8, FFAUDIO_F_INT16):
		for (i = 0;  i != samples;  i++) {
			for (ich = 0;  ich != nch;  ich++) {
				to.pi16[ich][i * ostep] = (((int)from.pu8[ich][i * istep]) - 127) * 0x100;
			}
		}
		break;

	case X(FFAUDIO_F_UINT8, FFAUDIO_F_INT32):
		for (i = 0;  i != samples;  i++) {
			for (ich = 0;  ich != nch;  ich++) {
				to.pi32[ich][i * ostep] = (((int)from.pu8[ich][i * istep]) - 127) * 0x1000000;
			}
		}
		break;

	case X(FFAUDIO_F_UINT8, FFAUDIO_F_FLOAT64):
		for (i = 0;  i != samples;  i++) {
			for (ich = 0;  ich != nch;  ich++) {
				to.pf64[ich][i * ostep] = pcm_flt_i8(((int)from.pu8[ich][i * istep]) - 127);
			}
		}
		break;

// int8
	case X(FFAUDIO_F_INT8, FFAUDIO_F_INT8):
		for (i = 0;  i != samples;  i++) {
			for (ich = 0;  ich != nch;  ich++) {
				to.pi8[ich][i * ostep] = from.pi8[ich][i * istep];
			}
		}
		break;

	case X(FFAUDIO_F_INT8, FFAUDIO_F_INT16):
		for (i = 0;  i != samples;  i++) {
			for (ich = 0;  ich != nch;  ich++) {
				to.pi16[ich][i * ostep] = (int)from.pi8[ich][i * istep] * 0x100;
			}
		}
		break;

	case X(FFAUDIO_F_INT8, FFAUDIO_F_INT32):
		for (i = 0;  i != samples;  i++) {
			for (ich = 0;  ich != nch;  ich++) {
				to.pi32[ich][i * ostep] = (int)from.pi8[ich][i * istep] * 0x1000000;
			}
		}
		break;

	case X(FFAUDIO_F_INT8, FFAUDIO_F_FLOAT32):
		for (i = 0;  i != samples;  i++) {
			for (ich = 0;  ich != nch;  ich++) {
				to.pf32[ich][i * ostep] = pcm_flt_i8((int)from.pi8[ich][i * istep]);
			}
		}
		break;

// int16
	case X(FFAUDIO_F_INT16, FFAUDIO_F_INT8):
		for (i = 0;  i != samples;  i++) {
			for (ich = 0;  ich != nch;  ich++) {
				to.pi8[ich][i * ostep] = from.pi16[ich][i * istep] / 0x100;
			}
		}
		break;

	case X(FFAUDIO_F_INT16, FFAUDIO_F_INT16):
		for (i = 0;  i != samples;  i++) {
			for (ich = 0;  ich != nch;  ich++) {
				to.pi16[ich][i * ostep] = from.pi16[ich][i * istep];
			}
		}
		break;

	case X(FFAUDIO_F_INT16, FFAUDIO_F_INT24):
		for (i = 0;  i != samples;  i++) {
			for (ich = 0;  ich != nch;  ich++) {
				pcm_i24_i32(&to.pi8[ich][i * ostep * 3], (int)from.pi16[ich][i * istep] * 0x100);
			}
		}
		break;

	case X(FFAUDIO_F_INT16, FFAUDIO_F_INT24_4):
		for (i = 0;  i != samples;  i++) {
			for (ich = 0;  ich != nch;  ich++) {
				to.pi8[ich][i * ostep * 4 + 0] = 0;
				pcm_i24_i32(&to.pi8[ich][i * ostep * 4 + 1], (int)from.pi16[ich][i * istep] * 0x100);
			}
		}
		break;

	case X(FFAUDIO_F_INT16, FFAUDIO_F_INT32):
		for (i = 0;  i != samples;  i++) {
			for (ich = 0;  ich != nch;  ich++) {
				to.pi32[ich][i * ostep] = (int)from.pi16[ich][i * istep] * 0x10000;
			}
		}
		break;

	case X(FFAUDIO_F_INT16, FFAUDIO_F_FLOAT32):
		for (i = 0;  i != samples;  i++) {
			for (ich = 0;  ich != nch;  ich++) {
				to.pf32[ich][i * ostep] = pcm_flt_i16(from.pi16[ich][i * istep]);
			}
		}
		break;

	case X(FFAUDIO_F_INT16, FFAUDIO_F_FLOAT64):
		for (i = 0;  i != samples;  i++) {
			for (ich = 0;  ich != nch;  ich++) {
				to.pf64[ich][i * ostep] = pcm_flt_i16(from.pi16[ich][i * istep]);
			}
		}
		break;

// int24
	case X(FFAUDIO_F_INT24, FFAUDIO_F_INT16):
		for (i = 0;  i != samples;  i++) {
			for (ich = 0;  ich != nch;  ich++) {
				to.pi16[ich][i * ostep] = ffint_le_cpu24_ptr(&from.pi8[ich][i * istep * 3]) / 0x100;
			}
		}
		break;


	case X(FFAUDIO_F_INT24, FFAUDIO_F_INT24):
		for (i = 0;  i != samples;  i++) {
			for (ich = 0;  ich != nch;  ich++) {
				memcpy(&to.pi8[ich][i * ostep * 3], &from.pi8[ich][i * istep * 3], 3);
			}
		}
		break;

	case X(FFAUDIO_F_INT24, FFAUDIO_F_INT24_4):
		for (i = 0;  i != samples;  i++) {
			for (ich = 0;  ich != nch;  ich++) {
				to.pi8[ich][i * ostep * 4 + 0] = 0;
				memcpy(&to.pi8[ich][i * ostep * 4 + 1], &from.pi8[ich][i * istep * 3], 3);
			}
		}
		break;

	case X(FFAUDIO_F_INT24, FFAUDIO_F_INT32):
		for (i = 0;  i != samples;  i++) {
			for (ich = 0;  ich != nch;  ich++) {
				to.pi32[ich][i * ostep] = ffint_le_cpu24_ptr(&from.pi8[ich][i * istep * 3]) * 0x100;
			}
		}
		break;

	case X(FFAUDIO_F_INT24, FFAUDIO_F_FLOAT32):
		for (i = 0;  i != samples;  i++) {
			for (ich = 0;  ich != nch;  ich++) {
				to.pf32[ich][i * ostep] = pcm_flt_i24(pcm_i32_i24(&from.pi8[ich][i * istep * 3]));
			}
		}
		break;

	case X(FFAUDIO_F_INT24, FFAUDIO_F_FLOAT64):
		for (i = 0;  i != samples;  i++) {
			for (ich = 0;  ich != nch;  ich++) {
				to.pf64[ich][i * ostep] = pcm_flt_i24(pcm_i32_i24(&from.pi8[ich][i * istep * 3]));
			}
		}
		break;

// int32
	case X(FFAUDIO_F_INT32, FFAUDIO_F_INT16):
		for (i = 0;  i != samples;  i++) {
			for (ich = 0;  ich != nch;  ich++) {
				to.pi16[ich][i * ostep] = from.pi32[ich][i * istep] / 0x10000;
			}
		}
		break;

	case X(FFAUDIO_F_INT32, FFAUDIO_F_INT24):
		for (i = 0;  i != samples;  i++) {
			for (ich = 0;  ich != nch;  ich++) {
				pcm_i24_i32(&to.pi8[ich][i * ostep * 3], from.pi32[ich][i * istep] / 0x100);
			}
		}
		break;

	case X(FFAUDIO_F_INT32, FFAUDIO_F_INT24_4):
		for (i = 0;  i != samples;  i++) {
			for (ich = 0;  ich != nch;  ich++) {
				to.pi8[ich][i * ostep * 4 + 0] = 0;
				pcm_i24_i32(&to.pi8[ich][i * ostep * 4 + 1], from.pi32[ich][i * istep] / 0x100);
			}
		}
		break;

	case X(FFAUDIO_F_INT32, FFAUDIO_F_INT32):
		for (i = 0;  i != samples;  i++) {
			for (ich = 0;  ich != nch;  ich++) {
				to.pi32[ich][i * ostep] = from.pi32[ich][i * istep];
			}
		}
		break;

	case X(FFAUDIO_F_INT32, FFAUDIO_F_FLOAT32):
		for (i = 0;  i != samples;  i++) {
			for (ich = 0;  ich != nch;  ich++) {
				to.pf32[ich][i * ostep] = pcm_flt_i32(from.pi32[ich][i * istep]);
			}
		}
		break;

// float32
	case X(FFAUDIO_F_FLOAT32, FFAUDIO_F_INT16):
		for (i = 0;  i != samples;  i++) {
			for (ich = 0;  ich != nch;  ich++) {
				to.pi16[ich][i * ostep] = pcm_i16_flt(from.pf32[ich][i * istep]);
			}
		}
		break;

	case X(FFAUDIO_F_FLOAT32, FFAUDIO_F_INT24):
		for (i = 0;  i != samples;  i++) {
			for (ich = 0;  ich != nch;  ich++) {
				pcm_i24_i32(&to.pi8[ich][i * ostep * 3], pcm_i24_flt(from.pf32[ich][i * istep]));
			}
		}
		break;

	case X(FFAUDIO_F_FLOAT32, FFAUDIO_F_INT24_4):
		for (i = 0;  i != samples;  i++) {
			for (ich = 0;  ich != nch;  ich++) {
				to.pi8[ich][i * ostep * 4 + 0] = 0;
				pcm_i24_i32(&to.pi8[ich][i * ostep * 4 + 1], pcm_i24_flt(from.pf32[ich][i * istep]));
			}
		}
		break;

	case X(FFAUDIO_F_FLOAT32, FFAUDIO_F_INT32):
		for (i = 0;  i != samples;  i++) {
			for (ich = 0;  ich != nch;  ich++) {
				to.pi32[ich][i * ostep] = pcm_i32_flt(from.pf32[ich][i * istep]);
			}
		}
		break;

	case X(FFAUDIO_F_FLOAT32, FFAUDIO_F_FLOAT32):
		for (i = 0;  i != samples;  i++) {
			for (ich = 0;  ich != nch;  ich++) {
				to.pf32[ich][i * ostep] = from.pf32[ich][i * istep];
			}
		}
		break;

	case X(FFAUDIO_F_FLOAT32, FFAUDIO_F_FLOAT64):
		for (i = 0;  i != samples;  i++) {
			for (ich = 0;  ich != nch;  ich++) {
				to.pf64[ich][i * ostep] = from.pf32[ich][i * istep];
			}
		}
		break;

// float64
	case X(FFAUDIO_F_FLOAT64, FFAUDIO_F_INT16):
		for (i = 0;  i != samples;  i++) {
			for (ich = 0;  ich != nch;  ich++) {
				to.pi16[ich][i * ostep] = pcm_i16_flt(from.pf64[ich][i * istep]);
			}
		}
		break;

	case X(FFAUDIO_F_FLOAT64, FFAUDIO_F_INT24):
		for (i = 0;  i != samples;  i++) {
			for (ich = 0;  ich != nch;  ich++) {
				pcm_i24_i32(&to.pi8[ich][i * ostep * 3], pcm_i24_flt(from.pf64[ich][i * istep]));
			}
		}
		break;

	case X(FFAUDIO_F_FLOAT64, FFAUDIO_F_INT32):
		for (i = 0;  i != samples;  i++) {
			for (ich = 0;  ich != nch;  ich++) {
				to.pi32[ich][i * ostep] = pcm_i32_flt(from.pf64[ich][i * istep]);
			}
		}
		break;

	case X(FFAUDIO_F_FLOAT64, FFAUDIO_F_FLOAT32):
		for (i = 0;  i != samples;  i++) {
			for (ich = 0;  ich != nch;  ich++) {
				to.pf32[ich][i * ostep] = from.pf64[ich][i * istep];
			}
		}
		break;

	case X(FFAUDIO_F_FLOAT64, FFAUDIO_F_FLOAT64):
		for (i = 0;  i != samples;  i++) {
			for (ich = 0;  ich != nch;  ich++) {
				to.pf64[ich][i * ostep] = from.pf64[ich][i * istep];
			}
		}
		break;

	default:
		r = -1;
		goto done;
	}

done:
	ffmem_free(tmpptr);
	return r;
}

#undef X
#undef X4
