/** ffaudio: OSS wrapper
2020, Simon Zolin
*/

#include <ffaudio/audio.h>
#include <ffaudio/util.h>
#include <ffbase/stringz.h>

#include <sys/soundcard.h>
#include <sys/ioctl.h> /* FreeBSD gets ioctl() via soundcard.h, Linux does not */
#include <fcntl.h>
#include <errno.h>
#include <math.h>


int ffoss_init(ffaudio_init_conf *conf)
{
	return 0;
}

void ffoss_uninit()
{
}


struct ffaudio_dev {
	char *id;
	char *name;
	oss_audioinfo ainfo;
	int mixer;
	ffuint ndevs;
	ffuint idx;
	ffuint mode;
	char errmsg[100];
	int err;
};

ffaudio_dev* ffoss_dev_alloc(ffuint mode)
{
	ffaudio_dev *d = ffmem_new(ffaudio_dev);
	if (d == NULL)
		return NULL;
	d->mixer = -1;
	d->mode = mode;
	return d;
}

void ffoss_dev_free(ffaudio_dev *d)
{
	if (d == NULL)
		return;
	if (d->mixer == -1)
		close(d->mixer);
	ffmem_free(d);
}

int ffoss_dev_next(ffaudio_dev *d)
{
	if (d->mixer == -1) {
		if (-1 == (d->mixer = open("/dev/mixer", O_RDONLY, 0))) {
			d->err = errno;
			return -FFAUDIO_ERROR;
		}

		oss_sysinfo si;
		ffmem_zero_obj(&si);
		ioctl(d->mixer, SNDCTL_SYSINFO, &si);
		d->ndevs = si.numaudios;
	}

	for (;;) {
		if (d->idx == d->ndevs)
			return 1;

		ffmem_zero_obj(&d->ainfo);
		d->ainfo.dev = d->idx++;
		if (0 > ioctl(d->mixer, SNDCTL_AUDIOINFO_EX, &d->ainfo)) {
			d->err = errno;
			return -FFAUDIO_ERROR;
		}

		if ((d->mode == FFAUDIO_DEV_PLAYBACK && (d->ainfo.caps & PCM_CAP_OUTPUT))
			|| (d->mode == FFAUDIO_DEV_CAPTURE && (d->ainfo.caps & PCM_CAP_INPUT)))
			break;
	}

	d->id = d->ainfo.devnode;
	d->name = d->ainfo.name;
	return 0;
}

const char* ffoss_dev_info(ffaudio_dev *d, ffuint i)
{
	switch (i) {
	case FFAUDIO_DEV_ID:
		return d->id;
	case FFAUDIO_DEV_NAME:
		return d->name;
	}
	return NULL;
}

const char* ffoss_dev_error(ffaudio_dev *d)
{
	d->errmsg[0] = '\0';
	strerror_r(d->err, d->errmsg, sizeof(d->errmsg));
	return d->errmsg;
}


struct ffaudio_buf {
	int fd;
	void *data;
	ffsize data_cap;
	ffuint frag_size;
	int nonblock;

	int err;
	const char *errfunc;
	char *errmsg;
};

ffaudio_buf* ffoss_alloc()
{
	ffaudio_buf *b = ffmem_new(ffaudio_buf);
	if (b == NULL)
		return NULL;
	b->fd = -1;
	return b;
}

void ffoss_free(ffaudio_buf *b)
{
	if (b == NULL)
		return;

	if (b->fd >= 0)
		close(b->fd);
	ffmem_free(b->errmsg);
	ffmem_free(b->data);
	ffmem_free(b);
}

const char* ffoss_error(ffaudio_buf *b)
{
	ffmem_free(b->errmsg);
	b->errmsg = ffsz_allocfmt("%s: (%d) %s", b->errfunc, b->err, strerror(b->err));
	return b->errmsg;
}

static int oss_fmt_from_ff(ffuint fmt)
{
	switch (fmt) {
	case FFAUDIO_F_INT8:
		return AFMT_S8;
	case FFAUDIO_F_INT16:
		return AFMT_S16_LE;
	case FFAUDIO_F_INT32:
		return AFMT_S32_LE;
	}
	return -1;
}

static int oss_fmt_to_ff(ffuint ossfmt)
{
	switch (ossfmt) {
	case AFMT_S8:
		return FFAUDIO_F_INT8;
	case AFMT_S16_LE:
		return FFAUDIO_F_INT16;
	case AFMT_S32_LE:
		return FFAUDIO_F_INT32;
	}
	return -1;
}

static int oss_set_format(ffaudio_buf *b, ffaudio_conf *conf)
{
	int r;
	int new_format = 0;

	int fmt_in, fmt_out;
	fmt_in = oss_fmt_from_ff(conf->format);
	fmt_out = (fmt_in != -1) ? fmt_in : AFMT_S16_LE;
	if (0 > ioctl(b->fd, SNDCTL_DSP_SETFMT, &fmt_out)) {
		b->errfunc = "ioctl(SNDCTL_DSP_SETFMT)";
		return FFAUDIO_ERROR;
	}
	if (fmt_out != fmt_in) {
		if (0 > (r = oss_fmt_to_ff(fmt_out))) {
			b->errfunc = "unsupported format";
			return FFAUDIO_ERROR;
		}
		conf->format = r;
		new_format = 1;
	}

	ffuint ch = conf->channels;
	if (0 > ioctl(b->fd, SNDCTL_DSP_CHANNELS, &ch)) {
		b->errfunc = "ioctl(SNDCTL_DSP_CHANNELS)";
		return FFAUDIO_ERROR;
	}
	if (ch != conf->channels) {
		conf->channels = ch;
		new_format = 1;
	}

	ffuint rate = conf->sample_rate;
	if (0 > ioctl(b->fd, SNDCTL_DSP_SPEED, &rate)) {
		b->errfunc = "ioctl(SNDCTL_DSP_SPEED)";
		return FFAUDIO_ERROR;
	}
	if (rate != conf->sample_rate) {
		conf->sample_rate = rate;
		new_format = 1;
	}

	if (new_format)
		return FFAUDIO_EFORMAT;
	return 0;
}

int ffoss_open(ffaudio_buf *b, ffaudio_conf *conf, ffuint flags)
{
	int r, rc = FFAUDIO_ERROR;
	ffuint f;
	int capture = (flags & 0x0f) == FFAUDIO_DEV_CAPTURE;
	b->nonblock = !!(flags & FFAUDIO_O_NONBLOCK);
	f = (!capture) ? O_WRONLY : O_RDONLY;
	f |= O_EXCL;
	f |= (b->nonblock) ? O_NONBLOCK : 0;
	const char *dev = conf->device_id;
	if (dev == NULL)
		dev = "/dev/dsp";
	if (0 > (b->fd = open(dev, f, 0))) {
		b->errfunc = "open";
		goto end;
	}

	/* Ask for the buffer geometry BEFORE the format is set.
	 *
	 * OSS allocates its DMA buffer when the format/channels/rate are set, and
	 * SNDCTL_DSP_SETFRAGMENT is silently ignored after that point.  Issuing it
	 * afterwards -- as this did -- meant the request never took effect and the
	 * device default was used instead: asking for 40 ms of capture returned
	 * 4 x 1024 = 10 ms, far too little for a process doing heavy DSP, where one
	 * scheduling delay past 10 ms drops samples.
	 *
	 * Size is derived from the requested format, which is what we want: the
	 * post-hoc GETISPACE the old code used could not run this early, and its
	 * integer division could also ask for zero fragments when the device
	 * fragment was larger than the whole request.
	 *
	 * Advisory: a device that refuses the hint still works with its own
	 * geometry, so a failure here is not fatal. */
	if (conf->buffer_length_msec != 0) {
		ffuint total = _ffau_buf_msec_to_size(conf, conf->buffer_length_msec);
		ffuint frag = 1024;
		while (frag * 8 < total && frag < 65536)
			frag <<= 1;
		ffuint num = total / frag;
		if (num < 4)
			num = 4;
		ffuint fr = (num << 16) | (ffuint)log2((double)frag);
		ioctl(b->fd, SNDCTL_DSP_SETFRAGMENT, &fr);
	}

	if (0 != (r = oss_set_format(b, conf))) {
		rc = r;
		goto end;
	}

	audio_buf_info info = {};
	ffmem_zero_obj(&info);
	if (capture) {
		r = ioctl(b->fd, SNDCTL_DSP_GETISPACE, &info);
	} else {
		r = ioctl(b->fd, SNDCTL_DSP_GETOSPACE, &info);
	}
	ffuint bufsize = 16*1024;
	if (r == 0) {
		b->frag_size = (info.fragsize > 0) ? (ffuint)info.fragsize : 0;
		conf->buffer_length_msec = _ffau_buf_size_to_msec(conf, info.fragstotal * info.fragsize);
		bufsize = info.fragstotal * info.fragsize;
	}

	if (NULL == (b->data = ffmem_alloc(bufsize))) {
		b->errfunc = "malloc";
		goto end;
	}
	b->data_cap = bufsize;

	return 0;

end:
	b->err = errno;
	if (b->fd >= 0) {
		close(b->fd);
		b->fd = -1;
	}
	return rc;
}

int ffoss_start(ffaudio_buf *b)
{
	return 0;
}

int ffoss_stop(ffaudio_buf *b)
{
	ioctl(b->fd, SNDCTL_DSP_HALT, 0);
	return 0;
}

int ffoss_clear(ffaudio_buf *b)
{
	return 0;
}

static int oss_writeonce(ffaudio_buf *b, const void *data, ffsize len)
{
	int r = write(b->fd, data, len);
	if (r < 0) {
		if (errno == EAGAIN)
			return 0;
		b->errfunc = "write";
		b->err = errno;
		return -FFAUDIO_ERROR;
	}
	return r;
}

int ffoss_write(ffaudio_buf *b, const void *data, ffsize len)
{
	return oss_writeonce(b, data, len);
}

int ffoss_drain(ffaudio_buf *b)
{
	if (0 > ioctl(b->fd, SNDCTL_DSP_SYNC, 0)) {
		b->errfunc = "ioctl(SNDCTL_DSP_SYNC)";
		b->err = errno;
		return -FFAUDIO_ERROR;
	}
	return 1;
}

static int oss_readonce(ffaudio_buf *b, const void **buffer)
{
	/* Read one fragment, not the whole buffer.
	 *
	 * Asking for data_cap makes the driver hand back whatever happens to be
	 * queued, which is almost always a ragged partial fragment -- and osscore's
	 * copy_to_user() fails intermittently on exactly those (dmesg shows
	 * copy_to_user(4032/3960/3408) against a 1024-byte fragment, while the one
	 * fragment-aligned size, 4096, is the exception).  PulseAudio and the other
	 * OSS clients ask a fragment at a time and never take that path.
	 *
	 * A fragment is also the granularity the driver signals at, so this costs
	 * no latency: it returns as soon as one is ready, rather than after the
	 * driver has scraped together a partial buffer. */
	ffsize want = (b->frag_size != 0 && b->frag_size <= b->data_cap)
		? b->frag_size : b->data_cap;
	int r = read(b->fd, b->data, want);
	if (r < 0) {
		if (errno == EAGAIN)
			return 0;
		b->errfunc = "read";
		b->err = errno;
		return -FFAUDIO_ERROR;
	}
	*buffer = b->data;
	return r;
}

int ffoss_read(ffaudio_buf *b, const void **buffer)
{
	return oss_readonce(b, buffer);
}


const struct ffaudio_interface ffoss = {
	ffoss_init,
	ffoss_uninit,

	ffoss_dev_alloc,
	ffoss_dev_free,
	ffoss_dev_error,
	ffoss_dev_next,
	ffoss_dev_info,

	ffoss_alloc,
	ffoss_free,
	ffoss_error,
	ffoss_open,
	ffoss_start,
	ffoss_stop,
	ffoss_clear,
	ffoss_write,
	ffoss_drain,
	ffoss_read,
	NULL,
};
