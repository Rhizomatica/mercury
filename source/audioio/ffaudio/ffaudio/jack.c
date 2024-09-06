/** ffaudio: JACK wrapper
2020, Simon Zolin
*/

#include <ffaudio/audio.h>
#include <ffbase/string.h>
#include <ffbase/ring.h>

#include <jack/jack.h>
#include <time.h>


static jack_client_t *gclient;

static void _jack_log(const char *s)
{
}

int ffjack_init(ffaudio_init_conf *conf)
{
	if (gclient != NULL) {
		conf->error = "already initialized";
		return FFAUDIO_ERROR;
	}

	jack_set_info_function(&_jack_log);
	jack_set_error_function(&_jack_log);

	if (conf->app_name == NULL)
		conf->app_name = "ffaudio";

	jack_status_t status;
	if (NULL == (gclient = jack_client_open(conf->app_name, JackNullOption, &status))) {
		conf->error = "jack_client_open";
		return FFAUDIO_ERROR;
	}
	return 0;
}

void ffjack_uninit()
{
	if (gclient == NULL)
		return;
	jack_client_close(gclient);
	gclient = NULL;
}


struct ffaudio_dev {
	ffuint mode;
	const char **names;
	ffuint idx;
};

ffaudio_dev* ffjack_dev_alloc(ffuint mode)
{
	ffaudio_dev *d = ffmem_new(ffaudio_dev);
	d->mode = mode;
	return d;
}

void ffjack_dev_free(ffaudio_dev *d)
{
	if (d == NULL)
		return;
	jack_free(d->names);
	ffmem_free(d);
}

int ffjack_dev_next(ffaudio_dev *d)
{
	if (d->names == NULL) {
		const char **portnames;
		ffuint mode = (d->mode == FFAUDIO_DEV_PLAYBACK) ? JackPortIsInput : JackPortIsOutput;
		if (NULL == (portnames = jack_get_ports(gclient, NULL, NULL, mode)))
			return 1;
		d->names = portnames;
		return 0;
	}

	d->idx++;
	if (d->names[d->idx] == NULL)
		return 1;
	return 0;
}

const char* ffjack_dev_info(ffaudio_dev *d, ffuint i)
{
	switch (i) {
	case FFAUDIO_DEV_ID:
		return d->names[d->idx];
	case FFAUDIO_DEV_NAME:
		return d->names[d->idx];
	}
	return NULL;
}

const char* ffjack_dev_error(ffaudio_dev *d)
{
	return "";
}


struct ffaudio_buf {
	jack_port_t *port;
	ffring *ring;
	ffring_head rhead;
	ffstr chunk;
	ffuint period_ms;
	ffuint started;
	ffuint shut;
	ffuint overrun;
	ffuint nonblock;

	const char *err;
};

ffaudio_buf* ffjack_alloc()
{
	ffaudio_buf *b = ffmem_new(ffaudio_buf);
	if (b == NULL)
		return NULL;
	return b;
}

static void _jack_close(ffaudio_buf *b)
{
	if (b->port != NULL)
		jack_port_unregister(gclient, b->port);
	ffring_free(b->ring);
}

void ffjack_free(ffaudio_buf *b)
{
	if (b == NULL)
		return;

	_jack_close(b);
	ffmem_free(b);
}

/** bytes -> msec:
size*1000/(rate*width*channels) */
static ffuint buffer_size_to_msec(const ffaudio_conf *conf, ffuint size)
{
	return size * 1000 / (conf->sample_rate * (conf->format & 0xff) / 8 * conf->channels);
}

static void _jack_shut(void *arg);
static int _jack_process(jack_nframes_t nframes, void *arg);

int ffjack_open(ffaudio_buf *b, ffaudio_conf *conf, ffuint flags)
{
	int rc = FFAUDIO_ERROR;
	const char **portnames = NULL;
	b->nonblock = !!(flags & FFAUDIO_O_NONBLOCK);

	ffuint rate = jack_get_sample_rate(gclient);
	if (conf->format != FFAUDIO_F_FLOAT32
		|| conf->sample_rate != rate
		|| conf->channels != 1) {

		conf->format = FFAUDIO_F_FLOAT32;
		conf->sample_rate = rate;
		conf->channels = 1;
		return FFAUDIO_EFORMAT;
	}

	jack_set_process_callback(gclient, &_jack_process, b);
	jack_on_shutdown(gclient, &_jack_shut, b);
	if (0 != jack_activate(gclient)) {
		b->err = "jack_activate";
		goto end;
	}

	if (NULL == (b->port = jack_port_register(gclient, conf->app_name, JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0))) {
		b->err = "jack_port_register";
		goto end;
	}

	const char *dev = conf->device_id;
	if (dev == NULL) {
		portnames = jack_get_ports(gclient, NULL, NULL, JackPortIsOutput);
		if (portnames == NULL) {
			b->err = "jack_get_ports";
			goto end;
		}
		dev = portnames[0];
	}

	if (0 != jack_connect(gclient, dev, jack_port_name(b->port))) {
		b->err = "jack_connect";
		goto end;
	}

	ffsize bufsize = jack_get_buffer_size(gclient);
	bufsize *= sizeof(float);
	conf->buffer_length_msec = buffer_size_to_msec(conf, bufsize);
	if (NULL == (b->ring = ffring_alloc(bufsize * 2, FFRING_1_READER | FFRING_1_WRITER))) {
		b->err = "ffring_create";
		goto end;
	}
	b->period_ms = conf->buffer_length_msec / 4;

	rc = 0;

end:
	jack_free(portnames);
	if (rc != 0)
		_jack_close(b);
	return rc;
}

int ffjack_start(ffaudio_buf *b)
{
	b->started = 1;
	return 0;
}

int ffjack_stop(ffaudio_buf *b)
{
	b->started = 0;
	return 0;
}

int ffjack_clear(ffaudio_buf *b)
{
	ffring_reset(b->ring);
	return 0;
}

static void _jack_shut(void *arg)
{
	ffaudio_buf *b = arg;
	b->shut = 1;
}

/** Called by JACK when new audio data is available */
static int _jack_process(jack_nframes_t nframes, void *arg)
{
	ffaudio_buf *b = arg;

	if (!b->started)
		return 0;

	const float *d = jack_port_get_buffer(b->port, nframes);
	ffsize n = nframes * sizeof(float);
	ffuint r = ffring_write(b->ring, d, n);
	if (r != n) {
		r += ffring_write(b->ring, (char*)d + r, n - r);
		if (r != n)
			b->overrun = 1;
	}

	return 0;
}

static int _jack_readonce(ffaudio_buf *b, const void **data)
{
	if (b->shut) {
		b->err = "shutdown";
		return -FFAUDIO_ERROR;
	}

	if (b->chunk.len != 0)
		ffring_read_finish(b->ring, b->rhead);

	b->rhead = ffring_read_begin(b->ring, -1, &b->chunk, NULL);
	if (b->chunk.len == 0) {
		if (!b->started)
			b->started = 1;
	}

	*data = b->chunk.ptr;
	return b->chunk.len;
}

int ffjack_write(ffaudio_buf *b, const void *data, ffsize len)
{
	b->err = "not supported";
	return -FFAUDIO_ERROR;
}

int ffjack_drain(ffaudio_buf *b)
{
	return 1;
}

static int _ff_sleep(ffuint msec)
{
	struct timespec ts = {
		.tv_sec = msec / 1000,
		.tv_nsec = (msec % 1000) * 1000000,
	};
	return nanosleep(&ts, NULL);
}

int ffjack_read(ffaudio_buf *b, const void **data)
{
	for (;;) {
		int r = _jack_readonce(b, data);
		if (r != 0)
			return r;

		if (b->nonblock)
			return 0;

		_ff_sleep(b->period_ms);
	}
}

const char* ffjack_error(ffaudio_buf *b)
{
	return b->err;
}

const struct ffaudio_interface ffjack = {
	ffjack_init,
	ffjack_uninit,

	ffjack_dev_alloc,
	ffjack_dev_free,
	ffjack_dev_error,
	ffjack_dev_next,
	ffjack_dev_info,

	ffjack_alloc,
	ffjack_free,
	ffjack_error,
	ffjack_open,
	ffjack_start,
	ffjack_stop,
	ffjack_clear,
	ffjack_write,
	ffjack_drain,
	ffjack_read,
	NULL,
};
