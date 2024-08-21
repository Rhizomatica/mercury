/** ffaudio: PulseAudio wrapper
2020, Simon Zolin
*/

#include <ffaudio/audio.h>
#include <ffbase/stringz.h>
#include <ffbase/atomic.h>
#include <pulse/pulseaudio.h>
#include <errno.h>


struct pulse_conn {
	pa_threaded_mainloop *mloop;
	pa_context *ctx;
	int cb_conn_state_change;
};

static struct pulse_conn *gconn;

static void pulse_uninit(struct pulse_conn *p);
static void pulse_on_conn_state_change(pa_context *c, void *udata);
static void pulse_wait(struct pulse_conn *p);

int ffpulse_init(ffaudio_init_conf *conf)
{
	struct pulse_conn *p;

	if (gconn != NULL) {
		conf->error = "already initialized";
		return FFAUDIO_ERROR;
	}

	if (NULL == (p = ffmem_new(struct pulse_conn))) {
		conf->error = "memory allocate";
		return FFAUDIO_ERROR;
	}

	if (NULL == (p->mloop = pa_threaded_mainloop_new())) {
		conf->error = "pa_threaded_mainloop_new";
		goto end;
	}

	if (conf->app_name == NULL)
		conf->app_name = "ffaudio";

	pa_mainloop_api *mlapi = pa_threaded_mainloop_get_api(p->mloop);
	if (NULL == (p->ctx = pa_context_new_with_proplist(mlapi, conf->app_name, NULL))) {
		conf->error = "pa_context_new_with_proplist";
		goto end;
	}

	if (0 != pa_context_connect(p->ctx, NULL, 0, NULL)) {
		conf->error = "pa_context_connect";
		goto end;
	}
	pa_context_set_state_callback(p->ctx, pulse_on_conn_state_change, p);

	if (0 != pa_threaded_mainloop_start(p->mloop)) {
		conf->error = "pa_threaded_mainloop_start";
		goto end;
	}

	pa_threaded_mainloop_lock(p->mloop);
	for (;;) {
		int r = pa_context_get_state(p->ctx);
		if (r == PA_CONTEXT_READY)
			break;
		else if (r == PA_CONTEXT_FAILED || r == PA_CONTEXT_TERMINATED) {
			conf->error = pa_strerror(pa_context_errno(p->ctx));
			pa_threaded_mainloop_unlock(p->mloop);
			goto end;
		}

		pulse_wait(p);
	}
	pa_threaded_mainloop_unlock(p->mloop);

	gconn = p;
	return 0;

end:
	pulse_uninit(p);
	return FFAUDIO_ERROR;
}

static void pulse_uninit(struct pulse_conn *p)
{
	if (p == NULL)
		return;

	if (p->ctx != NULL) {
		pa_threaded_mainloop_lock(p->mloop);
		pa_context_disconnect(p->ctx);
		pa_context_unref(p->ctx);
		pa_threaded_mainloop_unlock(p->mloop);
	}

	if (p->mloop != NULL) {
		pa_threaded_mainloop_stop(p->mloop);
		pa_threaded_mainloop_free(p->mloop);
	}

	ffmem_free(p);
}

void ffpulse_uninit()
{
	pulse_uninit(gconn);
	gconn = NULL;
}

static void pulse_lock(struct pulse_conn *conn)
{
	pa_threaded_mainloop_lock(conn->mloop);
}

static void pulse_unlock(struct pulse_conn *conn)
{
	pa_threaded_mainloop_unlock(conn->mloop);
}

static void pulse_wait(struct pulse_conn *conn)
{
	pa_threaded_mainloop_wait(conn->mloop);
}

/**
Return
 0: complete
 <0: would block
 FFAUDIO_ERROR: operation was cancelled
 FFAUDIO_ECONNECTION: connection failed */
static int pulse_op_wait(struct pulse_conn *conn, pa_operation *op, int nonblock, int *err)
{
	if (op == NULL) {
		*err = pa_context_errno(conn->ctx);
		if (PA_CONTEXT_READY != pa_context_get_state(conn->ctx))
			return FFAUDIO_ECONNECTION;
		return FFAUDIO_ERROR;
	}

	int r;
	for (;;) {
		r = pa_operation_get_state(op);
		if (r == PA_OPERATION_DONE) {
			r = 0;
			break;

		} else if (r == PA_OPERATION_CANCELLED) {
			*err = pa_context_errno(conn->ctx);

			if (PA_CONTEXT_READY != pa_context_get_state(conn->ctx)) {
				r = FFAUDIO_ECONNECTION;
				break;
			}

			r = FFAUDIO_ERROR;
			break;
		}

		if (nonblock)
			return -1;

		pulse_wait(conn);
	}

	pa_operation_unref(op);
	return r;
}

static void pulse_signal(struct pulse_conn *conn)
{
	pa_threaded_mainloop_signal(conn->mloop, 0);
}

// Called within mainloop thread after connection state with PA server changes
static void pulse_on_conn_state_change(pa_context *c, void *udata)
{
	struct pulse_conn *conn = udata;
	FFINT_WRITEONCE(conn->cb_conn_state_change, 1);
	ffcpu_fence_release();
	pulse_signal(conn);
}


struct dev_props {
	struct dev_props *next;
	char *id;
	char *name;
};

struct ffaudio_dev {
	struct pulse_conn *conn;
	ffuint mode;
	struct dev_props *head, *cur;
	int cb_dev_next;

	const char *errfunc;
	char *errmsg;
	int err;
};

static void pulse_dev_on_next_sink(pa_context *c, const pa_sink_info *info, int eol, void *udata);
static void pulse_dev_on_next_source(pa_context *c, const pa_source_info *info, int eol, void *udata);

ffaudio_dev* ffpulse_dev_alloc(ffuint mode)
{
	if (gconn == NULL)
		return NULL;
	ffaudio_dev *d = ffmem_new(ffaudio_dev);
	if (d == NULL)
		return NULL;
	d->mode = mode;
	d->conn = gconn;
	return d;
}

static void pulse_dev_free_chain(struct dev_props *head)
{
	struct dev_props *it, *next;
	for (it = head;  it != NULL;  it = next) {
		next = it->next;
		ffmem_free(it->id);
		ffmem_free(it->name);
		ffmem_free(it);
	}
}

void ffpulse_dev_free(ffaudio_dev *d)
{
	if (d == NULL)
		return;

	pulse_dev_free_chain(d->head);
	ffmem_free(d->errmsg);
	ffmem_free(d);
}

static void pulse_dev_on_next_sink(pa_context *c, const pa_sink_info *info, int eol, void *udata)
{
	ffaudio_dev *d = udata;
	if (eol > 0) {
		FFINT_WRITEONCE(d->cb_dev_next, 1);
		ffcpu_fence_release();
		pulse_signal(d->conn);
		return;
	}

	struct dev_props *p = ffmem_new(struct dev_props);
	if (p == NULL) {
		d->errfunc = "mem alloc";
		d->err = errno;
		return;
	}
	p->id = ffsz_dup(info->name);
	p->name = ffsz_dup(info->description);
	if (p->id == NULL || p->name == NULL) {
		d->errfunc = "mem alloc";
		d->err = errno;
		ffmem_free(p);
		return;
	}

	if (d->head == NULL)
		d->head = p;
	else
		d->cur->next = p;
	d->cur = p;
}

static void pulse_dev_on_next_source(pa_context *c, const pa_source_info *info, int eol, void *udata)
{
	ffaudio_dev *d = udata;
	if (eol > 0) {
		FFINT_WRITEONCE(d->cb_dev_next, 1);
		ffcpu_fence_release();
		pulse_signal(d->conn);
		return;
	}

	struct dev_props *p = ffmem_new(struct dev_props);
	if (p == NULL) {
		d->errfunc = "mem alloc";
		d->err = errno;
		return;
	}
	p->id = ffsz_dup(info->name);
	p->name = ffsz_dup(info->description);
	if (p->id == NULL || p->name == NULL) {
		d->errfunc = "mem alloc";
		d->err = errno;
		ffmem_free(p);
		return;
	}

	if (d->head == NULL)
		d->head = p;
	else
		d->cur->next = p;
	d->cur = p;
}

int ffpulse_dev_next(ffaudio_dev *d)
{
	if (d->head != NULL) {
		d->cur = d->cur->next;
		if (d->cur == NULL)
			return 1;
		return 0;
	}

	pulse_lock(d->conn);

	pa_operation *op;
	if (d->mode == FFAUDIO_DEV_PLAYBACK) {
		op = pa_context_get_sink_info_list(d->conn->ctx, pulse_dev_on_next_sink, d);
		d->errfunc = "pa_context_get_sink_info_list";
	} else {
		op = pa_context_get_source_info_list(d->conn->ctx, pulse_dev_on_next_source, d);
		d->errfunc = "pa_context_get_source_info_list";
	}
	pulse_op_wait(d->conn, op, 0, &d->err);

	pulse_unlock(d->conn);

	if (d->head == NULL) {
		if (d->err != 0)
			return -FFAUDIO_ERROR;
		return 1;
	}
	d->cur = d->head;
	return 0;
}

const char* ffpulse_dev_info(ffaudio_dev *d, ffuint i)
{
	switch (i) {
	case FFAUDIO_DEV_ID:
		return d->cur->id;
	case FFAUDIO_DEV_NAME:
		return d->cur->name;
	}
	return NULL;
}

const char* ffpulse_dev_error(ffaudio_dev *d)
{
	ffmem_free(d->errmsg);
	d->errmsg = ffsz_allocfmt("%s: %d", d->errfunc, d->err);
	return d->errmsg;
}


struct ffaudio_buf {
	struct pulse_conn *conn;
	pa_stream *stm;
	ffuint capture;
	ffuint buf_locked;
	ffuint nonblock;
	ffuint drained;
	pa_operation *drain_op;

	/** Remember the signals received by our PA callbacks
	1: I/O-signal
	2: stream-state-changed
	4: operation-complete
	*/
	ffuint cb_signals;

	int err; // pa_context_errno()
	const char *errfunc; // PA function name or ffaudio error message
	char *errmsg; // prepared error message
};

static int pulse_buf_op_wait(struct ffaudio_buf *b, pa_operation *op)
{
	return pulse_op_wait(b->conn, op, 0, &b->err);
}

static int pulse_buf_op_check(struct ffaudio_buf *b, pa_operation *op)
{
	return pulse_op_wait(b->conn, op, 1, &b->err);
}

ffaudio_buf* ffpulse_alloc()
{
	if (gconn == NULL)
		return NULL;
	ffaudio_buf *b = ffmem_new(ffaudio_buf);
	if (b == NULL)
		return NULL;
	b->conn = gconn;
	return b;
}

void ffpulse_free(ffaudio_buf *b)
{
	if (b == NULL)
		return;

	pulse_lock(b->conn);

	if (b->drain_op != NULL) {
		pa_operation_cancel(b->drain_op);
		b->drain_op = NULL;
	}

	if (b->stm != NULL) {
		pa_stream_disconnect(b->stm);
		pa_stream_set_state_callback(b->stm, NULL, NULL);
		pa_stream_set_write_callback(b->stm, NULL, NULL);
		pa_stream_set_read_callback(b->stm, NULL, NULL);
		pa_stream_unref(b->stm);
		b->stm = NULL;
	}

	pulse_unlock(b->conn);
	ffmem_free(b->errmsg);
	ffmem_free(b);
}

static const ffushort afmt[] = {
	FFAUDIO_F_UINT8,
	FFAUDIO_F_INT16,
	FFAUDIO_F_INT24,
	FFAUDIO_F_INT32,
	FFAUDIO_F_FLOAT32,
};
static const ffuint afmt_pa[] = {
	PA_SAMPLE_U8,
	PA_SAMPLE_S16LE,
	PA_SAMPLE_S24LE,
	PA_SAMPLE_S32LE,
	PA_SAMPLE_FLOAT32LE,
};

/** ffaudio format -> Pulse format */
static int pulse_fmt(ffuint f)
{
	int r;
	if (0 > (r = ffarrint16_find(afmt, FF_COUNT(afmt), f)))
		return -FFAUDIO_F_FLOAT32;
	return afmt_pa[r];
}

static void pulse_on_io(pa_stream *s, ffsize nbytes, void *udata);

/** PA manual: "called whenever the state of the stream changes" */
static void pulse_on_change(pa_stream *s, void *udata)
{
	ffaudio_buf *b = udata;
	b->cb_signals |= 2;
	pulse_signal(b->conn);
}

/** msec -> bytes:
rate*width*channels*msec/1000 */
static ffuint buffer_size(const ffaudio_conf *conf, ffuint msec)
{
	return conf->sample_rate * (conf->format & 0xff) / 8 * conf->channels * msec / 1000;
}

int ffpulse_open(ffaudio_buf *b, ffaudio_conf *conf, ffuint flags)
{
	if ((flags & 0x0f) > FFAUDIO_CAPTURE
		|| (flags & ~(0x0f | FFAUDIO_O_NONBLOCK))) {
		b->errfunc = "unsupported flags";
		b->err = 0;
		return FFAUDIO_ERROR;
	}

	int r = FFAUDIO_ERROR;
	b->nonblock = !!(flags & FFAUDIO_O_NONBLOCK);
	b->capture = ((flags & 0x0f) == FFAUDIO_CAPTURE);

	if (conf->buffer_length_msec == 0)
		conf->buffer_length_msec = 500;

	if (conf->app_name == NULL)
		conf->app_name = "ffaudio";

	if (0 > (r = pulse_fmt(conf->format))) {
		conf->format = -r;
		return FFAUDIO_EFORMAT;
	}

	pulse_lock(b->conn);

	pa_sample_spec spec;
	spec.format = r;
	spec.rate = conf->sample_rate;
	spec.channels = conf->channels;
	b->stm = pa_stream_new(b->conn->ctx, conf->app_name, &spec, NULL);
	if (b->stm == NULL) {
		b->errfunc = "pa_stream_new";
		goto end;
	}

	pa_buffer_attr attr;
	ffmem_fill(&attr, 0xff, sizeof(pa_buffer_attr));
	attr.tlength = buffer_size(conf, conf->buffer_length_msec);

	pa_stream_set_state_callback(b->stm, pulse_on_change, b);
	if (!b->capture) {
		pa_stream_set_write_callback(b->stm, pulse_on_io, b);
		pa_stream_connect_playback(b->stm, conf->device_id, &attr, 0, NULL, NULL);
		b->errfunc = "pa_stream_connect_playback";
	} else {
		pa_stream_set_read_callback(b->stm, pulse_on_io, b);
		pa_stream_connect_record(b->stm, conf->device_id, &attr, 0);
		b->errfunc = "pa_stream_connect_record";
	}

	for (;;) {
		r = pa_stream_get_state(b->stm);
		if (r == PA_STREAM_READY)
			break;
		else if (r == PA_STREAM_TERMINATED || r == PA_STREAM_FAILED) {
			goto end;
		}

		if (PA_CONTEXT_READY != (r = pa_context_get_state(b->conn->ctx))) {
			r = FFAUDIO_ECONNECTION;
			goto end;
		}

		pulse_wait(b->conn);
	}

	r = 0;

end:
	if (r != 0) {
		b->err = pa_context_errno(b->conn->ctx);
		if (b->stm != NULL) {
			pa_stream_disconnect(b->stm);
			pa_stream_unref(b->stm);
			b->stm = NULL;
		}
	}
	pulse_unlock(b->conn);
	return r;
}

static void pulse_on_op(pa_stream *s, int success, void *udata)
{
	ffaudio_buf *b = udata;
	b->cb_signals |= 4;
	pulse_signal(b->conn);
}

int pulse_resume(ffaudio_buf *b)
{
	if (!pa_stream_is_corked(b->stm))
		return 0;

	pa_operation *op = pa_stream_cork(b->stm, 0, pulse_on_op, b);
	b->errfunc = "pa_stream_cork";
	return pulse_buf_op_wait(b, op);
}

int ffpulse_start(ffaudio_buf *b)
{
	pulse_lock(b->conn);
	int r = pulse_resume(b);
	pulse_unlock(b->conn);
	return r;
}

int ffpulse_stop(ffaudio_buf *b)
{
	int r = 0;
	pulse_lock(b->conn);

	if (pa_stream_is_corked(b->stm))
		goto end;

	pa_operation *op = pa_stream_cork(b->stm, 1, pulse_on_op, b);
	b->errfunc = "pa_stream_cork";
	r = pulse_buf_op_wait(b, op);

end:
	pulse_unlock(b->conn);
	return r;
}

int ffpulse_clear(ffaudio_buf *b)
{
	pulse_lock(b->conn);

	pa_operation *op = pa_stream_flush(b->stm, pulse_on_op, b);
	b->errfunc = "pa_stream_flush";
	int r = pulse_buf_op_wait(b, op);

	if (r == 0 && b->drain_op != NULL) {
		pa_operation_cancel(b->drain_op);
		pa_operation_unref(b->drain_op);
		b->drain_op = NULL;
		b->drained = 1;
	}

	pulse_unlock(b->conn);
	return r;
}

static int pulse_writeonce(ffaudio_buf *b, const void *data, ffsize len)
{
	int r;
	ffsize n;
	void *buf;

	n = pa_stream_writable_size(b->stm);
	if (n == 0) {
		return 0;
	} else if (n == (ffsize)-1) {
		b->errfunc = "pa_stream_writable_size";
		b->err = pa_context_errno(b->conn->ctx);
		return -FFAUDIO_ERROR;
	}

	r = pa_stream_begin_write(b->stm, &buf, &n);
	if (r < 0 || buf == NULL) {
		b->errfunc = "pa_stream_begin_write";
		b->err = pa_context_errno(b->conn->ctx);
		return -FFAUDIO_ERROR;
	}
	n = ffmin(len, n);

	ffmem_copy(buf, data, n);

	if (0 != pa_stream_write(b->stm, buf, n, NULL, 0, PA_SEEK_RELATIVE)) {
		b->errfunc = "pa_stream_write";
		b->err = pa_context_errno(b->conn->ctx);
		return -FFAUDIO_ERROR;
	}

	b->drained = 0;
	return n;
}

static void pulse_on_io(pa_stream *s, ffsize nbytes, void *udata)
{
	ffaudio_buf *b = udata;
	b->cb_signals |= 1;
	pulse_signal(b->conn);
}

static int pulse_readonce(ffaudio_buf *b, const void **data)
{
	for (;;) {
		if (b->buf_locked) {
			b->buf_locked = 0;
			pa_stream_drop(b->stm);
		}

		ffsize len;
		if (0 != pa_stream_peek(b->stm, data, &len)) {
			b->errfunc = "pa_stream_peek";
			b->err = pa_context_errno(b->conn->ctx);
			return -FFAUDIO_ERROR;
		}
		b->buf_locked = 1;

		if (*data == NULL && len != 0) {
			// data hole
			continue;
		}

		return len;
	}
}

int ffpulse_write(ffaudio_buf *b, const void *data, ffsize len)
{
	int r;
	pulse_lock(b->conn);

	for (;;) {
		r = pulse_writeonce(b, data, len);
		if (r != 0)
			goto end;

		if (0 != (r = pulse_resume(b))) {
			r = -r;
			goto end;
		}

		if (b->nonblock)
			goto end;

		pulse_wait(b->conn);
	}

end:
	pulse_unlock(b->conn);
	return r;
}

int ffpulse_drain(ffaudio_buf *b)
{
	if (b->drained)
		return 1;

	pulse_lock(b->conn);

	int r;
	if (0 != (r = pulse_resume(b))) {
		r = -r;
		goto end;
	}

	pa_operation *op = b->drain_op;
	if (op == NULL) {
		op = pa_stream_drain(b->stm, pulse_on_op, b);
	}

	if (!b->nonblock) {
		if (0 != (r = pulse_buf_op_wait(b, op))) {
			b->errfunc = "pa_stream_drain";
			r = -r;
			goto end;
		}

	} else {
		r = pulse_buf_op_check(b, op);
		if (r > 0) {
			b->errfunc = "pa_stream_drain";
			r = -r;
			goto end;
		} else if (r < 0) {
			b->drain_op = op;
			r = 0;
			goto end;
		}
	}

	b->drain_op = NULL;
	b->drained = 1;
	r = 1;

end:
	pulse_unlock(b->conn);
	return r;
}

int ffpulse_read(ffaudio_buf *b, const void **data)
{
	int r;
	pulse_lock(b->conn);

	for (;;) {
		r = pulse_readonce(b, data);
		if (r != 0)
			goto end;

		if (b->nonblock)
			goto end;

		pulse_wait(b->conn);
	}

end:
	pulse_unlock(b->conn);
	return r;
}

/*
Note: libpulse's code calls _exit() when it fails to allocate a memory buffer (/src/pulse/xmalloc.c) */
const char* ffpulse_error(ffaudio_buf *b)
{
	if (b->err == 0)
		return b->errfunc;
	ffmem_free(b->errmsg);
	b->errmsg = ffsz_allocfmt("%s: (%u) %s", b->errfunc, b->err, pa_strerror(b->err));
	return b->errmsg;
}


const struct ffaudio_interface ffpulse = {
	ffpulse_init,
	ffpulse_uninit,

	ffpulse_dev_alloc,
	ffpulse_dev_free,
	ffpulse_dev_error,
	ffpulse_dev_next,
	ffpulse_dev_info,

	ffpulse_alloc,
	ffpulse_free,
	ffpulse_error,
	ffpulse_open,
	ffpulse_start,
	ffpulse_stop,
	ffpulse_clear,
	ffpulse_write,
	ffpulse_drain,
	ffpulse_read,
	NULL,
};
