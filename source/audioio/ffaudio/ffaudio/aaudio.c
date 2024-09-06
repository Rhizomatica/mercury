/** ffaudio: AAudio
2023, Simon Zolin
*/

#include <ffaudio/audio.h>
#include <ffbase/ring.h>
#include <aaudio/AAudio.h>

static int ffaaudio_init(ffaudio_init_conf *conf)
{
	return 0;
}

static void ffaaudio_uninit()
{
}


struct ffaudio_buf {
	AAudioStream *as;
	ffring *ring;
	ffring_head rhead;
	ffstr buf_locked;
	ffuint frame_size;
	ffuint capture :1;
	ffuint notify_unsync :1;
	ffuint overrun :1;
	ffuint nonblock :1;
	int dev_error;
	ffuint period_ms;

	void (*on_event)(void*);
	void *udata;

	const char *err;
	int errcode;
	char *errmsg;
};

static ffaudio_buf* ffaaudio_alloc()
{
	ffaudio_buf *b = ffmem_new(ffaudio_buf);
	if (b == NULL)
		return NULL;
	return b;
}

static void ffaaudio_free(ffaudio_buf *b)
{
	if (b == NULL) return;

	AAudioStream_close(b->as);
	ffring_free(b->ring);
	ffmem_free(b->errmsg);
	ffmem_free(b);
}

static unsigned buffer_frames_to_msec(const ffaudio_conf *conf, unsigned frames)
{
	return frames * 1000 / conf->sample_rate;
}

static unsigned buffer_msec_to_frames(const ffaudio_conf *conf, unsigned msec)
{
	return conf->sample_rate * msec / 1000;
}

/** msec -> bytes:
rate*width*channels*msec/1000 */
static ffuint buffer_msec_to_size(const ffaudio_conf *conf, ffuint msec)
{
	return conf->sample_rate * (conf->format & 0xff) / 8 * conf->channels * msec / 1000;
}

static void on_event_dummy(void *param) {}
static aaudio_data_callback_result_t on_play(AAudioStream *stream, void *userData, void *audioData, int32_t numFrames);
static aaudio_data_callback_result_t on_capture(AAudioStream *stream, void *userData, void *audioData, int32_t numFrames);

static void on_error(AAudioStream *stream, void *userData, aaudio_result_t error)
{
	ffaudio_buf *b = userData;
	b->dev_error = error;
	b->on_event(b->udata);
}

static int fmt_aa_ffa(ffuint ffa)
{
	switch (ffa) {
	case FFAUDIO_F_INT16: return AAUDIO_FORMAT_PCM_I16;
	case FFAUDIO_F_FLOAT32: return AAUDIO_FORMAT_PCM_FLOAT;
	}
	return AAUDIO_FORMAT_PCM_FLOAT;
}

static ffuint fmt_fa_aa(int aa)
{
	switch (aa) {
	case AAUDIO_FORMAT_PCM_I16: return FFAUDIO_F_INT16;
	case AAUDIO_FORMAT_PCM_FLOAT: return FFAUDIO_F_FLOAT32;
	}
	return 0;
}

static int ffaaudio_open(ffaudio_buf *b, ffaudio_conf *conf, ffuint flags)
{
	int rc = FFAUDIO_ERROR, r;
	AAudioStreamBuilder *asb;
	if (0 != (r = AAudio_createStreamBuilder(&asb))) {
		b->err = "AAudio_createStreamBuilder()";
		b->errcode = r;
		goto end;
	}

	b->capture = ((flags & 0x0f) == FFAUDIO_CAPTURE);
	b->notify_unsync = !!(flags & FFAUDIO_O_UNSYNC_NOTIFY);
	b->nonblock = !!(flags & FFAUDIO_O_NONBLOCK);

	if (b->capture)
		AAudioStreamBuilder_setDirection(asb, AAUDIO_DIRECTION_INPUT);

	if (conf->format != 0)
		AAudioStreamBuilder_setFormat(asb, fmt_aa_ffa(conf->format));

	if (conf->sample_rate != 0)
		AAudioStreamBuilder_setSampleRate(asb, conf->sample_rate);

	if (conf->channels != 0)
		AAudioStreamBuilder_setChannelCount(asb, conf->channels);

	unsigned msec = (conf->buffer_length_msec != 0) ? conf->buffer_length_msec : 500;
	unsigned frames = buffer_msec_to_frames(conf, msec);
	AAudioStreamBuilder_setBufferCapacityInFrames(asb, frames);

	if (flags & FFAUDIO_O_EXCLUSIVE)
		AAudioStreamBuilder_setSharingMode(asb, AAUDIO_SHARING_MODE_EXCLUSIVE);

	if (flags & (FFAUDIO_O_POWER_SAVE | FFAUDIO_O_LOW_LATENCY)) {
		ffuint pm = (flags & FFAUDIO_O_POWER_SAVE) ? AAUDIO_PERFORMANCE_MODE_POWER_SAVING : AAUDIO_PERFORMANCE_MODE_LOW_LATENCY;
		AAudioStreamBuilder_setPerformanceMode(asb, pm);
	}

	b->on_event = (conf->on_event != NULL) ? conf->on_event : on_event_dummy;
	b->udata = conf->udata;
	AAudioStreamBuilder_setDataCallback(asb, (!b->capture) ? on_play : on_capture, b);
	AAudioStreamBuilder_setErrorCallback(asb, on_error, b);

	if (0 != (r = AAudioStreamBuilder_openStream(asb, &b->as))) {
		b->err = "AAudioStreamBuilder_openStream()";
		b->errcode = r;
		goto end;
	}

	if (flags & FFAUDIO_O_EXCLUSIVE) {
		if (AAUDIO_SHARING_MODE_EXCLUSIVE != AAudioStream_getSharingMode(b->as)) {
			b->err = "can't set exclusive mode";
			b->errcode = 0;
			goto end;
		}
	}

	ffuint ufmt = conf->format;
	ffuint uchan = conf->channels;
	ffuint urate = conf->sample_rate;
	int fmt = AAudioStream_getFormat(b->as);
	if (0 == (conf->format = fmt_fa_aa(fmt))) {
		b->errcode = 0;
		b->err = "format not supported";
		goto end;
	}
	conf->channels = AAudioStream_getChannelCount(b->as);
	conf->sample_rate = AAudioStream_getSampleRate(b->as);
	if (!((ufmt == 0 || conf->format == ufmt)
		&& (uchan == 0 || conf->channels == uchan)
		&& (urate == 0 || conf->sample_rate == urate))) {
		rc = FFAUDIO_EFORMAT;
		goto end;
	}

	r = AAudioStream_getBufferSizeInFrames(b->as);
	conf->buffer_length_msec = buffer_frames_to_msec(conf, r);
	b->period_ms = conf->buffer_length_msec / 4;
	b->frame_size = (conf->format & 0xff) / 8 * conf->channels;

	ffuint bufsize = buffer_msec_to_size(conf, conf->buffer_length_msec);
	if (NULL == (b->ring = ffring_alloc(bufsize, FFRING_1_WRITER))) {
		b->err = "ffring_alloc()";
		b->errcode = 0;
		goto end;
	}

	rc = 0;

end:
	if (rc != 0) {
		AAudioStream_close(b->as);
		b->as = NULL;
	}
	AAudioStreamBuilder_delete(asb);
	return rc;
}

static int ffaaudio_start(ffaudio_buf *b)
{
	int r = AAudioStream_getState(b->as);
	if (!(r == AAUDIO_STREAM_STATE_STARTING
			|| r == AAUDIO_STREAM_STATE_STARTED)) {
		if (0 != (r = AAudioStream_requestStart(b->as))) {
			b->err = "AAudioStream_requestStart()";
			b->errcode = r;
			return FFAUDIO_ERROR;
		}
	}
	return 0;
}

static int ffaaudio_stop(ffaudio_buf *b)
{
	int r;

	if (!b->capture) {
		if (0 != (r = AAudioStream_requestPause(b->as))) {
			b->err = "AAudioStream_requestPause()";
			goto err;
		}
	} else {
		if (0 != (r = AAudioStream_requestStop(b->as))) {
			b->err = "AAudioStream_requestStop()";
			goto err;
		}
	}

	return 0;

err:
	b->errcode = r;
	return FFAUDIO_ERROR;
}

static int ffaaudio_clear(ffaudio_buf *b)
{
	ffring_read_discard(b->ring);

	int st = AAudioStream_getState(b->as);
	if (st == AAUDIO_STREAM_STATE_PAUSING)
		AAudioStream_waitForStateChange(b->as, st, &st, 3000000000ULL);

	int r = AAudioStream_requestFlush(b->as);
	if (r != 0) {
		b->err = "AAudioStream_requestFlush()";
		b->errcode = r;
		return FFAUDIO_ERROR;
	}

	return 0;
}

static aaudio_data_callback_result_t on_capture(AAudioStream *stream, void *userData, void *audioData, int32_t numFrames)
{
	ffaudio_buf *b = userData;
	ffuint n = numFrames * b->frame_size;
	ffuint r = ffring_write(b->ring, audioData, n);
	if (r != n) {
		r += ffring_write(b->ring, (char*)audioData + r, n - r);
		b->overrun = (r != n);
	}
	b->on_event(b->udata);
	return 0;
}

static int aaudio_readsome(ffaudio_buf *b, const void **buffer)
{
	if (b->buf_locked.len != 0) {
		ffring_read_finish(b->ring, b->rhead);
	}

	b->rhead = ffring_read_begin(b->ring, -1, &b->buf_locked, NULL);
	*buffer = b->buf_locked.ptr;
	return b->buf_locked.len;
}

static aaudio_data_callback_result_t on_play(AAudioStream *stream, void *userData, void *audioData, int32_t numFrames)
{
	ffaudio_buf *b = userData;
	b->on_event(b->udata);

	u_char *d = audioData;
	unsigned n = numFrames * b->frame_size;
	ffstr s;
	ffring_head h = ffring_read_begin(b->ring, n, &s, NULL);
	if (s.len == 0)
		goto end;
	ffmem_copy(d, s.ptr, s.len);
	ffring_read_finish(b->ring, h);

	d += s.len;
	n -= s.len;
	if (n != 0) {
		h = ffring_read_begin(b->ring, n, &s, NULL);
		if (s.len == 0)
			goto end;
		ffmem_copy(d, s.ptr, s.len);
		ffring_read_finish(b->ring, h);
	}
	return 0;

end:
	if (n != 0) {
		ffmem_fill(d, 0, n);
		b->overrun = 1;
	}
	return 0;
}

static int aaudio_write_some(ffaudio_buf *b, const void *data, size_t len)
{
	unsigned r = ffring_write(b->ring, data, len);
	if (r != len) {
		r += ffring_write(b->ring, (char*)data + r, len - r);
	}
	return r;
}

static int sleep_msec(unsigned msec)
{
	struct timespec ts = {
		.tv_sec = msec / 1000,
		.tv_nsec = (msec % 1000) * 1000000,
	};
	return nanosleep(&ts, NULL);
}

static int io_error(ffaudio_buf *b)
{
	b->err = "I/O error";
	b->errcode = b->dev_error;
	if (b->dev_error == AAUDIO_ERROR_DISCONNECTED)
		return -FFAUDIO_EDEV_OFFLINE;
	return -FFAUDIO_ERROR;
}

static int ffaaudio_write(ffaudio_buf *b, const void *data, ffsize len)
{
	for (;;) {

		if (b->dev_error != 0) {
			return io_error(b);
		}

		if (b->notify_unsync && b->overrun) {
			b->overrun = 0;
			b->err = "buffer underrun";
			b->errcode = 0;
			return -FFAUDIO_ESYNC;
		}

		int r = aaudio_write_some(b, data, len);
		if (r != 0)
			return r;

		if (0 != (r = ffaaudio_start(b)))
			return -r;

		if (b->nonblock)
			return 0;

		sleep_msec(b->period_ms);
	}
}

static int ffaaudio_drain(ffaudio_buf *b)
{
	int r;
	for (;;) {

		if (b->dev_error != 0) {
			return io_error(b);
		}

		ffstr s;
		size_t free;
		ffring_write_begin(b->ring, 0, &s, &free);

		if (free == b->ring->cap) {
			(void) ffaaudio_stop(b);
			return 1;
		}

		if (0 != (r = ffaaudio_start(b)))
			return r;

		if (b->nonblock)
			return 0;

		sleep_msec(b->period_ms);
	}
}

static int ffaaudio_read(ffaudio_buf *b, const void **buffer)
{
	for (;;) {
		int r = aaudio_readsome(b, buffer);
		if (r != 0)
			return r;

		if (b->dev_error != 0) {
			return io_error(b);
		}

		if (b->notify_unsync && b->overrun) {
			b->overrun = 0;
			b->err = "buffer overrun";
			b->errcode = 0;
			return -FFAUDIO_ESYNC;
		}

		if (0 != (r = ffaaudio_start(b)))
			return -r;

		if (b->nonblock)
			return 0;

		sleep_msec(b->period_ms);
	}
}

static const char* ffaaudio_error(ffaudio_buf *b)
{
	ffmem_free(b->errmsg);
	b->errmsg = ffsz_allocfmt("%s: (%d) %s"
		, b->err, b->errcode, AAudio_convertResultToText(b->errcode));
	return b->errmsg;
}

const struct ffaudio_interface ffaaudio = {
	ffaaudio_init, ffaaudio_uninit,

	NULL,
	NULL,
	NULL,
	NULL,
	NULL,

	ffaaudio_alloc, ffaaudio_free,
	ffaaudio_error,
	ffaaudio_open,
	ffaaudio_start, ffaaudio_stop,
	ffaaudio_clear,
	ffaaudio_write, ffaaudio_drain,
	ffaaudio_read,
	NULL,
};
