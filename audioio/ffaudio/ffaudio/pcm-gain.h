/** ffaudio: volume change.
2015, Simon Zolin */

#include <ffaudio/audio.h>
#include <ffaudio/pcm.h>

static inline int pcm_gain(const struct pcm_af *af, double gain, const void *in, void *out, uint samples)
{
	uint i, ich, step = 1, nch = af->channels;
	void *ini[PCM_CHAN_MAX], *oni[PCM_CHAN_MAX];
	union pcm_data from, to;

	if (gain == 1)
		return 0;

	if (af->channels > PCM_CHAN_MAX)
		return -1;

	from.p = (void*)in;
	to.p = out;

	if (af->interleaved) {

		if (af->channels == 2) {
			switch (af->format) {
			case FFAUDIO_F_FLOAT32:
				for (i = 0;  i < samples;  i++) {
					*to.f32++ = *from.f32++ * gain;
					*to.f32++ = *from.f32++ * gain;
				}
				return 0;
			}
		}

		from.pi8 = pcm_setni(ini, from.i8, af->format, nch);
		to.pi8 = pcm_setni(oni, to.i8, af->format, nch);
		step = nch;
	}

	switch (af->format) {
	case FFAUDIO_F_INT8:
		for (ich = 0;  ich != nch;  ich++) {
			for (i = 0;  i != samples;  i++) {
				to.pi8[ich][i * step] = pcm_i8_flt(pcm_flt_i8(from.pi8[ich][i * step]) * gain);
			}
		}
		break;

	case FFAUDIO_F_INT16:
		for (ich = 0;  ich != nch;  ich++) {
			for (i = 0;  i != samples;  i++) {
				to.pi16[ich][i * step] = pcm_i16_flt(pcm_flt_i16(from.pi16[ich][i * step]) * gain);
			}
		}
		break;

	case FFAUDIO_F_INT24:
		for (ich = 0;  ich != nch;  ich++) {
			for (i = 0;  i != samples;  i++) {
				int n = pcm_i32_i24(&from.pi8[ich][i * step * 3]);
				pcm_i24_i32(&to.pi8[ich][i * step * 3], pcm_i24_flt(pcm_flt_i24(n) * gain));
			}
		}
		break;

	case FFAUDIO_F_INT32:
		for (ich = 0;  ich != nch;  ich++) {
			for (i = 0;  i != samples;  i++) {
				to.pi32[ich][i * step] = pcm_i32_flt(pcm_flt_i32(from.pi32[ich][i * step]) * gain);
			}
		}
		break;

	case FFAUDIO_F_FLOAT32:
		for (ich = 0;  ich != nch;  ich++) {
			for (i = 0;  i != samples;  i++) {
				to.pf32[ich][i * step] = from.pf32[ich][i * step] * gain;
			}
		}
		break;

	case FFAUDIO_F_FLOAT64:
		for (ich = 0;  ich != nch;  ich++) {
			for (i = 0;  i != samples;  i++) {
				to.pf64[ich][i * step] = from.pf64[ich][i * step] * gain;
			}
		}
		break;

	default:
		return -1;
	}

	return 0;
}
