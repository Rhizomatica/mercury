/** ffaudio: utilitary functions
2025, Simon Zolin */

/** Bits per sample */
#define _ffau_f_bits(f)  ((f) & 0xff)

/* Convert bytes <-> msec:
msec = size * 1000 / (rate * width * channels)
*/

static inline unsigned _ffau_buf_size_to_msec(const ffaudio_conf *conf, unsigned size) {
	return size * 1000 / (conf->sample_rate * _ffau_f_bits(conf->format)/8 * conf->channels);
}

static inline unsigned _ffau_buf_msec_to_size(const ffaudio_conf *conf, unsigned msec) {
	return conf->sample_rate * _ffau_f_bits(conf->format)/8 * conf->channels * msec / 1000;
}
