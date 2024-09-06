/** ffaudio: ALSA
2020, Simon Zolin
*/

#include <ffaudio/audio.h>
#include <ffbase/string.h>
#include <ffbase/stringz.h>

#include <alsa/asoundlib.h>
#include <time.h>


int ffalsa_init(ffaudio_init_conf *conf)
{
	return 0;
}

void ffalsa_uninit()
{
}


struct ffaudio_dev {
	ffuint mode;
	ffuint state;
	int sc;
	int idev;
	snd_ctl_t *sctl;
	snd_ctl_card_info_t *scinfo;

	const char *errfunc;
	int errcode;
	char *errmsg;

	char id[64];
	char *name;
};

ffaudio_dev* ffalsa_dev_alloc(ffuint mode)
{
	ffaudio_dev *d = ffmem_new(ffaudio_dev);
	if (d == NULL)
		return NULL;
	d->mode = mode;
	d->sc = -1;
	return d;
}

void ffalsa_dev_free(ffaudio_dev *d)
{
	if (d == NULL)
		return;

	if (d->scinfo != NULL)
		snd_ctl_card_info_free(d->scinfo);
	if (d->sctl != NULL)
		snd_ctl_close(d->sctl);
	ffmem_free(d->name);
	ffmem_free(d->errmsg);
	ffmem_free(d);
}

int ffalsa_dev_next(ffaudio_dev *d)
{
	int e, stream;
	char scard[32];
	enum { I_CARD, I_DEV };
	snd_pcm_info_t *pcminfo;
	snd_pcm_info_alloca(&pcminfo);

	if (d->scinfo == NULL
		&& 0 != (e = snd_ctl_card_info_malloc(&d->scinfo))) {
		d->errfunc = "snd_ctl_card_info_malloc";
		d->errcode = e;
		return -FFAUDIO_ERROR;
	}

	for (;;) {
	switch (d->state) {

	case I_CARD:
		if (0 != (e = snd_card_next(&d->sc))) {
			d->errfunc = "snd_card_next";
			d->errcode = e;
			return -FFAUDIO_ERROR;
		}
		if (d->sc == -1)
			return 1; // done

		(void) ffs_format(scard, sizeof(scard), "hw:%u%Z", d->sc);
		if (d->sctl != NULL) {
			snd_ctl_close(d->sctl);
			d->sctl = NULL;
		}
		if (0 != snd_ctl_open(&d->sctl, scard, 0))
			continue;

		if (0 != (e = snd_ctl_card_info(d->sctl, d->scinfo))) {
			d->errfunc = "snd_ctl_card_info";
			d->errcode = e;
			return -FFAUDIO_ERROR;
		}
		d->idev = -1;
		// fallthrough

	case I_DEV:
		if (0 != snd_ctl_pcm_next_device(d->sctl, &d->idev)
			|| d->idev == -1) {
			d->state = I_CARD;
			continue;
		}

		snd_pcm_info_set_device(pcminfo, d->idev);
		stream = (d->mode == FFAUDIO_DEV_PLAYBACK) ? SND_PCM_STREAM_PLAYBACK : SND_PCM_STREAM_CAPTURE;
		snd_pcm_info_set_stream(pcminfo, stream);

		if (0 != (e = snd_ctl_pcm_info(d->sctl, pcminfo))) {
			d->state = I_CARD;
			continue;
		}

		ffmem_free(d->name);
		if (NULL == (d->name = ffsz_allocfmt("%s %s%Z"
			, snd_ctl_card_info_get_name(d->scinfo), snd_pcm_info_get_name(pcminfo)))) {
			d->state = I_CARD;
			continue;
		}

		(void) ffs_format(d->id, sizeof(d->id), "plughw:%u,%u%Z", d->sc, d->idev);
		return 0;
	}
	}
}

const char* ffalsa_dev_info(ffaudio_dev *d, ffuint i)
{
	switch (i) {
	case FFAUDIO_DEV_ID:
		return d->id;
	case FFAUDIO_DEV_NAME:
		return d->name;
	}
	return NULL;
}

const char* ffalsa_dev_error(ffaudio_dev *d)
{
	ffmem_free(d->errmsg);
	d->errmsg = ffsz_allocfmt("%s: %d", d->errfunc, d->errcode);
	return d->errmsg;
}


struct ffaudio_buf {
	snd_pcm_t *pcm;
	ffuint frame_size;
	ffuint period_ms;
	ffuint bufsize;
	ffuint channels;
	ffuint nonblock;

	snd_pcm_uframes_t mmap_frames;
	snd_pcm_uframes_t mmap_off;

	int retcode;
	const char *errfunc; // libALSA function name
	int err; // libALSA error code
	char *errmsg; // prepared error message
};

ffaudio_buf* ffalsa_alloc()
{
	ffaudio_buf *b = ffmem_new(ffaudio_buf);
	if (b == NULL)
		return NULL;
	return b;
}

void ffalsa_free(ffaudio_buf *b)
{
	if (b == NULL)
		return;

	if (b->pcm != NULL)
		snd_pcm_close(b->pcm);
	ffmem_free(b->errmsg);
	ffmem_free(b);
}

static const ffushort fmts[] = {
	FFAUDIO_F_INT8,
	FFAUDIO_F_UINT8,
	FFAUDIO_F_INT16,
	FFAUDIO_F_INT24,
	FFAUDIO_F_INT32,
	FFAUDIO_F_FLOAT32,
	FFAUDIO_F_FLOAT64,
};
static const ffuint alsa_fmts[] = {
	SND_PCM_FORMAT_S8,
	SND_PCM_FORMAT_U8,
	SND_PCM_FORMAT_S16_LE,
	SND_PCM_FORMAT_S24_3LE,
	SND_PCM_FORMAT_S32_LE,
	SND_PCM_FORMAT_FLOAT_LE,
	SND_PCM_FORMAT_FLOAT64_LE,
};

static int alsa_find_format(ffuint f)
{
	int r;
	if (0 > (r = ffarrint16_find(fmts, FF_COUNT(fmts), f)))
		return -FFAUDIO_F_INT16;
	return alsa_fmts[r];
}

/** Get the most compatible format supported by device */
static int alsa_find_best_format(snd_pcm_hw_params_t *params, ffuint format)
{
	snd_pcm_format_mask_t *mask;
	snd_pcm_format_mask_alloca(&mask);
	snd_pcm_hw_params_get_format_mask(params, mask);

	for (int i = FF_COUNT(alsa_fmts)-1;  i >= 0;  i--) {
		if (snd_pcm_format_mask_test(mask, alsa_fmts[i]))
			return fmts[i];
	}
	return -1;
}

static int alsa_apply_format(ffaudio_buf *b, snd_pcm_hw_params_t *params, ffaudio_conf *conf)
{
	int e;
	ffuint change = 0;

	int format = alsa_find_format(conf->format);
	if (format < 0) {
		conf->format = -format;
		change = 1;

	} else if (0 != snd_pcm_hw_params_set_format(b->pcm, params, format)) {
		if (0 < (format = alsa_find_best_format(params, format)))
			conf->format = format;
		change = 1;
	}

	ffuint ch = conf->channels;
	if (0 != (e = snd_pcm_hw_params_set_channels_near(b->pcm, params, &ch))) {
		b->errfunc = "snd_pcm_hw_params_set_channels_near";
		b->err = e;
		return FFAUDIO_ERROR;
	}
	if (ch != conf->channels) {
		if (ch > 2) {
			b->errfunc = "channels >2 are not supported";
			return FFAUDIO_ERROR;
		}
		conf->channels = ch;
		change = 1;
	}

	ffuint rate = conf->sample_rate;
	if (0 != (e = snd_pcm_hw_params_set_rate_near(b->pcm, params, &rate, 0))) {
		b->errfunc = "snd_pcm_hw_params_set_rate_near";
		b->err = e;
		return FFAUDIO_ERROR;
	}
	if (rate != conf->sample_rate) {
		conf->sample_rate = rate;
		change = 1;
	}

	if (change)
		return FFAUDIO_EFORMAT;

	return 0;
}

/** usec -> bytes:
rate*width*channels*usec/1000 */
static ffuint buffer_usec_to_size(const ffaudio_conf *conf, ffuint usec)
{
	return conf->sample_rate * (conf->format & 0xff) / 8 * conf->channels * usec / 1000000;
}

int ffalsa_open(ffaudio_buf *b, ffaudio_conf *conf, ffuint flags)
{
	snd_pcm_hw_params_t *params;
	int rc = FFAUDIO_ERROR;
	int e;
	b->nonblock = !!(flags & FFAUDIO_O_NONBLOCK);

	b->errfunc = NULL;

	snd_pcm_hw_params_alloca(&params);

	const char *dev = conf->device_id;
	if (dev == NULL || dev[0] == '\0')
		dev = "plughw:0,0";

	if ((flags & FFAUDIO_O_HWDEV) && ffsz_matchz(dev, "plug"))
		dev += FFS_LEN("plug");

	int mode = ((flags & 0x0f) == FFAUDIO_DEV_PLAYBACK) ? SND_PCM_STREAM_PLAYBACK : SND_PCM_STREAM_CAPTURE;
	if (0 != (e = snd_pcm_open(&b->pcm, dev, mode, 0/*SND_PCM_NONBLOCK*/))) {
		b->errfunc = "snd_pcm_open";
		b->err = e;
		goto end;
	}

	if (0 > (e = snd_pcm_hw_params_any(b->pcm, params))) {
		b->errfunc = "snd_pcm_hw_params_any";
		b->err = e;
		goto end;
	}

	int access = SND_PCM_ACCESS_MMAP_INTERLEAVED;
	if (0 != (e = snd_pcm_hw_params_set_access(b->pcm, params, access))) {
		b->errfunc = "snd_pcm_hw_params_set_access";
		b->err = e;
		goto end;
	}

	if (0 != (e = alsa_apply_format(b, params, conf))) {
		rc = e;
		goto end;
	}

	if (conf->buffer_length_msec == 0)
		conf->buffer_length_msec = 500;
	ffuint bufsize_usec = conf->buffer_length_msec * 1000;
	if (0 != (e = snd_pcm_hw_params_set_buffer_time_near(b->pcm, params, &bufsize_usec, NULL))) {
		b->errfunc = "snd_pcm_hw_params_set_buffer_time_near";
		b->err = e;
		goto end;
	}

	if (0 != (e = snd_pcm_hw_params(b->pcm, params))) {
		b->errfunc = "snd_pcm_hw_params";
		b->err = e;
		goto end;
	}

	b->frame_size = (conf->format & 0xff) / 8 * conf->channels;
	conf->buffer_length_msec = bufsize_usec / 1000;
	b->period_ms = conf->buffer_length_msec / 3;
	b->bufsize = buffer_usec_to_size(conf, bufsize_usec);
	b->channels = conf->channels;

	return 0;

end:
	if (b->pcm != NULL) {
		snd_pcm_close(b->pcm);
		b->pcm = NULL;
	}
	b->retcode = rc;
	return rc;
}

static int _ff_sleep(ffuint msec)
{
	struct timespec ts = {
		.tv_sec = msec / 1000,
		.tv_nsec = (msec % 1000) * 1000000,
	};
	return nanosleep(&ts, NULL);
}

static int alsa_handle_error(ffaudio_buf *b, int r)
{
	switch (r) {

	case -EINTR:
		return 0;

	case -ESTRPIPE:
		while (-EAGAIN == (r = snd_pcm_resume(b->pcm))) {
			_ff_sleep(b->period_ms);
		}
		if (r == 0)
			return 0;
		// fallthrough

	case -EPIPE:
		if (0 > (r = snd_pcm_prepare(b->pcm)))
			return r;
		return 0;
	}

	return r;
}

int alsa_start(ffaudio_buf *b)
{
	int r = snd_pcm_state(b->pcm);
	if (r == SND_PCM_STATE_RUNNING)
		return 0;

	else if (r == SND_PCM_STATE_PAUSED) {
		if (0 != (r = snd_pcm_pause(b->pcm, 0))) {
			b->errfunc = "snd_pcm_pause";
			b->err = r;
			return FFAUDIO_ERROR;
		}

	} else {
		if (0 != (r = snd_pcm_start(b->pcm))) {
			b->errfunc = "snd_pcm_start";
			b->err = r;
			return FFAUDIO_ERROR;
		}
	}
	return 0;
}

int ffalsa_start(ffaudio_buf *b)
{
	if (0 != alsa_start(b)) {
		if (b->err == -EPIPE)
			return 0;
		if (0 != alsa_handle_error(b, b->err))
			return FFAUDIO_ERROR;
		return alsa_start(b);
	}
	return 0;
}

int ffalsa_stop(ffaudio_buf *b)
{
	int r = snd_pcm_state(b->pcm);
	if (r != SND_PCM_STATE_RUNNING)
		return 0;

	if (0 != (r = snd_pcm_pause(b->pcm, 1))) {
		b->errfunc = "snd_pcm_pause";
		b->err = r;
		return FFAUDIO_ERROR;
	}
	return 0;
}

int ffalsa_clear(ffaudio_buf *b)
{
	int r = snd_pcm_state(b->pcm);

	if (r == SND_PCM_STATE_PAUSED) {
		if (0 != (r = snd_pcm_pause(b->pcm, 0))) {
			b->errfunc = "snd_pcm_pause";
			b->err = r;
			return FFAUDIO_ERROR;
		}
	} else if (r == SND_PCM_STATE_XRUN) {
		return 0;
	}

	if (0 != (r = snd_pcm_reset(b->pcm))) {
		b->errfunc = "snd_pcm_reset";
		b->err = r;
		return FFAUDIO_ERROR;
	}
	return 0;
}

static int alsa_writeonce(ffaudio_buf *b, const void *data, ffsize len)
{
	int e;
	const snd_pcm_channel_area_t *areas;
	snd_pcm_sframes_t r;
	snd_pcm_uframes_t frames;
	snd_pcm_uframes_t off;

	if (0 > (r = snd_pcm_avail_update(b->pcm))) { // needed for snd_pcm_mmap_begin()
		b->errfunc = "snd_pcm_avail_update";
		b->err = r;
		return -FFAUDIO_ERROR;
	}

	frames = len / b->frame_size;
	if (0 != (e = snd_pcm_mmap_begin(b->pcm, &areas, &off, &frames))) {
		b->errfunc = "snd_pcm_mmap_begin";
		b->err = e;
		return -FFAUDIO_ERROR;
	}

	if (frames == 0) {
		return 0;
	}

	ffmem_copy((char*)areas[0].addr + off * areas[0].step/8
		, (char*)data
		, frames * b->frame_size);

	r = snd_pcm_mmap_commit(b->pcm, off, frames);
	if (r >= 0 && (snd_pcm_uframes_t)r != frames)
		r = -EPIPE;
	if (r < 0) {
		b->errfunc = "snd_pcm_mmap_commit";
		b->err = r;
		return -FFAUDIO_ERROR;
	}

	return frames * b->frame_size;
}

static int alsa_readonce(ffaudio_buf *b, const void **data)
{
	int e;
	const snd_pcm_channel_area_t *areas;
	snd_pcm_sframes_t wr;

	if (b->mmap_frames != 0) {
		wr = snd_pcm_mmap_commit(b->pcm, b->mmap_off, b->mmap_frames);
		if (wr >= 0 && (snd_pcm_uframes_t)wr != b->mmap_frames)
			wr = -EPIPE;
		b->mmap_frames = 0;
		if (wr < 0) {
			b->errfunc = "snd_pcm_mmap_commit";
			b->err = wr;
			return -FFAUDIO_ERROR;
		}
	}

	if (0 > (wr = snd_pcm_avail_update(b->pcm))) { // needed for snd_pcm_mmap_begin()
		b->errfunc = "snd_pcm_avail_update";
		b->err = wr;
		return -FFAUDIO_ERROR;
	}

	b->mmap_frames = b->bufsize / b->frame_size;
	if (0 != (e = snd_pcm_mmap_begin(b->pcm, &areas, &b->mmap_off, &b->mmap_frames))) {
		b->errfunc = "snd_pcm_mmap_begin";
		b->err = e;
		return -FFAUDIO_ERROR;
	}

	if (b->mmap_frames == 0)
		return 0;

	*data = (char*)areas[0].addr + b->mmap_off * areas[0].step/8;

	return b->mmap_frames * b->frame_size;
}

int ffalsa_write(ffaudio_buf *b, const void *data, ffsize len)
{
	for (;;) {
		int r = alsa_writeonce(b, data, len);
		if (r > 0) {
			return r;
		} else if (r == 0) {
			r = alsa_start(b);
		}

		if (r != 0) {
			if (0 != alsa_handle_error(b, b->err))
				break;
			continue;
		}

		if (b->nonblock)
			return 0;

		_ff_sleep(b->period_ms);
	}

	if (b->err == -ENODEV)
		return -FFAUDIO_EDEV_OFFLINE;
	return -FFAUDIO_ERROR;
}

int ffalsa_drain(ffaudio_buf *b)
{
	for (;;) {
		snd_pcm_sframes_t r = snd_pcm_avail_update(b->pcm);
		if (r <= 0)
			return 1;

		if (0 != ffalsa_start(b))
			return -FFAUDIO_ERROR;

		if (b->nonblock)
			return 0;

		_ff_sleep(b->period_ms);
	}
}

int ffalsa_read(ffaudio_buf *b, const void **data)
{
	for (;;) {
		int r = alsa_readonce(b, data);
		if (r > 0) {
			return r;
		} else if (r < 0) {
			if (0 == alsa_handle_error(b, b->err))
				continue;
			break;
		}

		if (0 != ffalsa_start(b))
			break;

		if (b->nonblock)
			return 0;

		_ff_sleep(b->period_ms);
	}

	if (b->err == -ENODEV)
		return -FFAUDIO_EDEV_OFFLINE;
	return -FFAUDIO_ERROR;
}

const char* ffalsa_error(ffaudio_buf *b)
{
	ffmem_free(b->errmsg);
	b->errmsg = ffsz_allocfmt("%s: (%d) %s"
		, b->errfunc, b->err, snd_strerror(b->err));
	return b->errmsg;
}


const struct ffaudio_interface ffalsa = {
	ffalsa_init,
	ffalsa_uninit,

	ffalsa_dev_alloc,
	ffalsa_dev_free,
	ffalsa_dev_error,
	ffalsa_dev_next,
	ffalsa_dev_info,

	ffalsa_alloc,
	ffalsa_free,
	ffalsa_error,
	ffalsa_open,
	ffalsa_start,
	ffalsa_stop,
	ffalsa_clear,
	ffalsa_write,
	ffalsa_drain,
	ffalsa_read,
	NULL,
};
