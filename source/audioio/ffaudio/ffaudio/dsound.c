/** ffaudio: DirectSound wrapper
2020, Simon Zolin
*/

#include <ffaudio/audio.h>
#include <ffbase/string.h>

#include <dsound.h>


int ffdsound_init(ffaudio_init_conf *conf)
{
	return 0;
}

void ffdsound_uninit()
{
}


struct dsound_devinfo {
	struct dsound_devinfo *next;
	GUID *id, guid;
	char name[0];
};

struct ffaudio_dev {
	ffuint mode;
	struct dsound_devinfo *head, *cur;

	const char *errfunc;
	char *errmsg;
	ffuint err;
};

ffaudio_dev* ffdsound_dev_alloc(ffuint mode)
{
	ffaudio_dev *d = ffmem_new(ffaudio_dev);
	if (d == NULL)
		return NULL;
	d->mode = mode;
	return d;
}

static void dsound_dev_free_chain(struct dsound_devinfo *head)
{
	struct dsound_devinfo *it, *next;
	for (it = head;  it != NULL;  it = next) {
		next = it->next;
		ffmem_free(it);
	}
}

void ffdsound_dev_free(ffaudio_dev *d)
{
	if (d == NULL)
		return;

	dsound_dev_free_chain(d->head);
	ffmem_free(d->errmsg);
	ffmem_free(d);
}

/** Add one more device
Prepare object chain: info1 -> info2 -> NULL */
static BOOL CALLBACK dsound_devenum(GUID *guid, const wchar_t *desc, const wchar_t *sguid, void *udata)
{
	ffaudio_dev *d = udata;
	ffsize r = ffwsz_len(desc) * 4 + 1;

	struct dsound_devinfo *info = ffmem_alloc(sizeof(struct dsound_devinfo) + r);
	if (info == NULL) {
		d->err = GetLastError();
		return 0;
	}
	ffsz_wtou(info->name, r, desc);

	info->id = NULL;
	if (guid != NULL) {
		info->guid = *guid;
		info->id = &info->guid;
	}

	info->next = NULL;
	if (d->head == NULL)
		d->head = info;
	else
		d->cur->next = info;
	d->cur = info;
	return 1;
}

int ffdsound_dev_next(ffaudio_dev *d)
{
	if (d->head != NULL) {
		d->cur = d->cur->next;
		if (d->cur == NULL)
			return 1;
		return 0;
	}

	HRESULT r;
	if (d->mode == FFAUDIO_DEV_PLAYBACK) {
		if (0 != (r = DirectSoundEnumerateW(&dsound_devenum, d))) {
			d->errfunc = "DirectSoundEnumerate";
			goto end;
		}
	} else {
		if (0 != (r = DirectSoundCaptureEnumerateW(&dsound_devenum, d))) {
			d->errfunc = "DirectSoundCaptureEnumerate";
			goto end;
		}
	}

	if (d->head == NULL)
		return 1;
	d->cur = d->head;
	return 0;

end:
	d->err = r;
	dsound_dev_free_chain(d->head);
	d->head = NULL;
	return -FFAUDIO_ERROR;
}

const char* ffdsound_dev_info(ffaudio_dev *d, ffuint i)
{
	if (d->cur == NULL)
		return NULL;

	switch (i) {
	case FFAUDIO_DEV_ID:
		return (char*)d->cur->id;
	case FFAUDIO_DEV_NAME:
		return d->cur->name;
	}
	return NULL;
}

static char* dsound_error(const char *errfunc, ffuint err)
{
	wchar_t buf[255];
	int n = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS | FORMAT_MESSAGE_MAX_WIDTH_MASK
		, 0, err, 0, buf, FF_COUNT(buf), 0);
	if (n == 0)
		buf[0] = '\0';

	return ffsz_allocfmt("%s: %d (0x%xu) %q"
		, errfunc, err, err, buf);
}

const char* ffdsound_dev_error(ffaudio_dev *d)
{
	ffmem_free(d->errmsg);
	d->errmsg = dsound_error(d->errfunc, d->err);
	return d->errmsg;
}


struct ffaudio_buf {
	IDirectSound8 *play_dev;
	IDirectSoundBuffer *play_buf;

	IDirectSoundCapture *capt_dev;
	IDirectSoundCaptureBuffer *capt_buf;

	ffstr bufs[2];
	ffuint ibuf;
	ffuint bufsize;
	ffuint bufpos;
	ffuint period_ms;
	ffuint have_data;
	ffuint last_playpos;
	ffuint last_filled;
	ffuint drained;
	ffuint nonblock;

	const char *errfunc;
	ffuint err;
	char *errmsg;
};

ffaudio_buf* ffdsound_alloc()
{
	ffaudio_buf *b = ffmem_new(ffaudio_buf);
	if (b == NULL)
		return NULL;
	b->last_filled = -1;
	return b;
}

void ffdsound_free(ffaudio_buf *b)
{
	if (b == NULL)
		return;

	if (b->play_buf != NULL)
		IDirectSoundBuffer_Release(b->play_buf);
	if (b->play_dev != NULL)
		IDirectSound8_Release(b->play_dev);
	if (b->capt_buf != NULL)
		IDirectSoundCaptureBuffer_Release(b->capt_buf);
	if (b->capt_dev != NULL)
		IDirectSoundCapture_Release(b->capt_dev);
	ffmem_free(b->errmsg);
	ffmem_free(b);
}

const char* ffdsound_error(ffaudio_buf *b)
{
	ffmem_free(b->errmsg);
	b->errmsg = dsound_error(b->errfunc, b->err);
	return b->errmsg;
}

/** ffaudio format -> WAVEFORMATEX */
static void wf_from_ff(WAVEFORMATEX *wf, const ffaudio_conf *conf)
{
	wf->wFormatTag = 1;
	switch (conf->format) {
	case FFAUDIO_F_FLOAT32:
		wf->wFormatTag = 3;
		break;
	}

	wf->wBitsPerSample = conf->format & 0xff;
	wf->nSamplesPerSec = conf->sample_rate;
	wf->nChannels = conf->channels;
	wf->nBlockAlign = (conf->format & 0xff) / 8 * conf->channels;
	wf->nAvgBytesPerSec = conf->sample_rate * wf->nBlockAlign;
	wf->cbSize = 0;
}

/** msec -> bytes:
rate*width*channels*msec/1000 */
static ffuint buffer_msec_to_size(const ffaudio_conf *conf, ffuint msec)
{
	return conf->sample_rate * (conf->format & 0xff) / 8 * conf->channels * msec / 1000;
}

static int dsound_open_play(ffaudio_buf *b, ffaudio_conf *conf, ffuint flags)
{
	int r;

	if (0 != (r = DirectSoundCreate8((GUID*)conf->device_id, &b->play_dev, 0))) {
		b->errfunc = "DirectSoundCreate8";
		b->err = r;
		return FFAUDIO_ERROR;
	}

	HWND h = GetDesktopWindow();
	if (0 != (r = IDirectSound8_SetCooperativeLevel(b->play_dev, h, DSSCL_NORMAL))) {
		b->errfunc = "IDirectSound8_SetCooperativeLevel";
		b->err = r;
		goto end;
	}

	WAVEFORMATEX wf;
	wf_from_ff(&wf, conf);

	if (conf->buffer_length_msec == 0)
		conf->buffer_length_msec = 500;
	b->bufsize = buffer_msec_to_size(conf, conf->buffer_length_msec);

	DSBUFFERDESC bufdesc = {
		sizeof(DSBUFFERDESC),
		DSBCAPS_GLOBALFOCUS,
		b->bufsize, 0, &wf,
		{0}
	};

	if (0 != (r = IDirectSound8_CreateSoundBuffer(b->play_dev, &bufdesc, &b->play_buf, NULL))) {
		b->errfunc = "IDirectSound8_CreateSoundBuffer";
		b->err = r;
		goto end;
	}

	b->period_ms = conf->buffer_length_msec / 4;
	return 0;

end:
	if (b->play_buf != NULL) {
		IDirectSoundBuffer_Release(b->play_buf);
		b->play_buf = NULL;
	}
	if (b->play_dev != NULL) {
		IDirectSound8_Release(b->play_dev);
		b->play_dev = NULL;
	}
	return FFAUDIO_ERROR;
}

static int dsound_open_capt(ffaudio_buf *b, ffaudio_conf *conf, ffuint flags)
{
	int r;

	if (0 != (r = DirectSoundCaptureCreate((GUID*)conf->device_id, &b->capt_dev, NULL))) {
		b->errfunc = "DirectSoundCaptureCreate";
		b->err = r;
		return FFAUDIO_ERROR;
	}

	WAVEFORMATEX wf;
	wf_from_ff(&wf, conf);

	if (conf->buffer_length_msec == 0)
		conf->buffer_length_msec = 500;
	b->bufsize = buffer_msec_to_size(conf, conf->buffer_length_msec);

	DSCBUFFERDESC desc = {
		sizeof(DSCBUFFERDESC), 0, b->bufsize, 0, &wf, 0, NULL
	};
	if (0 != (r = IDirectSoundCapture_CreateCaptureBuffer(b->capt_dev, &desc, &b->capt_buf, NULL))) {
		b->errfunc = "IDirectSoundCapture_CreateCaptureBuffer";
		b->err = r;
		goto end;
	}

	b->period_ms = conf->buffer_length_msec / 4;
	return 0;

end:
	if (b->capt_buf != NULL) {
		IDirectSoundCaptureBuffer_Release(b->capt_buf);
		b->capt_buf = NULL;
	}
	if (b->capt_dev != NULL) {
		IDirectSoundCapture_Release(b->capt_dev);
		b->capt_dev = NULL;
	}
	return FFAUDIO_ERROR;
}

int ffdsound_open(ffaudio_buf *b, ffaudio_conf *conf, ffuint flags)
{
	b->nonblock = !!(flags & FFAUDIO_O_NONBLOCK);
	if ((flags & 0x0f) == FFAUDIO_DEV_PLAYBACK)
		return dsound_open_play(b, conf, flags);
	return dsound_open_capt(b, conf, flags);
}

int ffdsound_start(ffaudio_buf *b)
{
	int r;

	if (b->play_buf != NULL) {
		if (0 != (r = IDirectSoundBuffer_Play(b->play_buf, 0, 0, DSBPLAY_LOOPING))) {
			b->errfunc = "IDirectSoundBuffer_Play";
			b->err = r;
			return FFAUDIO_ERROR;
		}

	} else {
		if (0 != (r = IDirectSoundCaptureBuffer_Start(b->capt_buf, DSCBSTART_LOOPING))) {
			b->errfunc = "IDirectSoundCaptureBuffer_Start";
			b->err = r;
			return FFAUDIO_ERROR;
		}
	}

	return 0;
}

int ffdsound_stop(ffaudio_buf *b)
{
	int r;

	if (b->play_buf != NULL) {
		if (0 != (r = IDirectSoundBuffer_Stop(b->play_buf))) {
			b->errfunc = "IDirectSoundBuffer_Stop";
			b->err = r;
			return FFAUDIO_ERROR;
		}

	} else {
		if (0 != (r = IDirectSoundCaptureBuffer_Stop(b->capt_buf))) {
			b->errfunc = "IDirectSoundCaptureBuffer_Stop";
			b->err = r;
			return FFAUDIO_ERROR;
		}
	}

	return 0;
}

int ffdsound_clear(ffaudio_buf *b)
{
	if (b->play_buf != NULL) {
		b->last_playpos = 0;
		b->have_data = 0;
		(void) IDirectSoundBuffer_SetCurrentPosition(b->play_buf, 0);
	}
	return 0;
}

static int dsound_filled(ffaudio_buf *b)
{
	int r;
	DWORD playpos;
	if (0 != (r = IDirectSoundBuffer_GetCurrentPosition(b->play_buf, &playpos, NULL))) {
		b->errfunc = "IDirectSoundBuffer_GetCurrentPosition";
		b->err = r;
		return -FFAUDIO_ERROR;
	}

	if (b->last_playpos != playpos) {
		b->last_playpos = playpos;
		b->have_data = 0;
	}

	ffuint w = b->bufpos;
	ffuint R = playpos;
	ffuint filled = (R > w) ? b->bufsize - (R - w) // xx(w)..(r)xx
		: (R < w) ? w - R // ..(r)xx(w)..
		: (!b->have_data) ? 0 // ..(rw)....
		: b->bufsize; // xx(rw)xxxx

	return filled;
}

static int dsound_writeonce(ffaudio_buf *b, const void *data, ffsize len)
{
	int r;

	if (0 > (r = dsound_filled(b)))
		return r;

	ffuint n = b->bufsize - r;
	n = (ffuint)ffmin(len, n);
	if (n == 0)
		return 0;

	void *buf1, *buf2;
	DWORD len1, len2;
	if (0 != (r = IDirectSoundBuffer_Lock(b->play_buf, b->bufpos, n, &buf1, &len1, &buf2, &len2, 0))) {
		b->errfunc = "IDirectSoundBuffer_Lock";
		b->err = r;
		return -FFAUDIO_ERROR;
	}

	ffmem_copy(buf1, data, len1);
	if (len2 != 0)
		ffmem_copy(buf2, (char*)data + len1, len2);

	if (0 != (r = IDirectSoundBuffer_Unlock(b->play_buf, buf1, len1, buf2, len2))) {
		b->errfunc = "IDirectSoundBuffer_Unlock";
		b->err = r;
		return -FFAUDIO_ERROR;
	}

	b->bufpos = (b->bufpos + len1 + len2) % b->bufsize;
	b->have_data = 1;
	return n;
}

static int dsound_readonce(ffaudio_buf *b, const void **buffer)
{
	HRESULT r;
	void *ptr0, *ptr1;
	DWORD len0, len1;

	if (b->ibuf == 1) {
		b->ibuf = 2;
		if (b->bufs[1].len != 0) {
			*buffer = b->bufs[1].ptr;
			return b->bufs[1].len;
		}
	}

	if (b->ibuf == 2) {
		if (0 != (r = IDirectSoundCaptureBuffer_Unlock(b->capt_buf, b->bufs[0].ptr, b->bufs[0].len, b->bufs[1].ptr, b->bufs[1].len))) {
			b->errfunc = "IDirectSoundCaptureBuffer_Unlock";
			b->err = r;
			return -FFAUDIO_ERROR;
		}
		b->ibuf = 0;
	}

	DWORD pos;
	if (0 != (r = IDirectSoundCaptureBuffer_GetCurrentPosition(b->capt_buf, NULL, &pos))) {
		b->errfunc = "IDirectSoundCaptureBuffer_GetCurrentPosition";
		b->err = r;
		return -FFAUDIO_ERROR;
	}
	ffuint filled = (pos >= b->bufpos) ? pos - b->bufpos : b->bufsize - b->bufpos + pos;
	if (filled == 0)
		return 0;

	if (0 != (r = IDirectSoundCaptureBuffer_Lock(b->capt_buf, b->bufpos, filled, &ptr0, &len0, &ptr1, &len1, 0))) {
		b->errfunc = "IDirectSoundCaptureBuffer_Lock";
		b->err = r;
		return -FFAUDIO_ERROR;
	}

	ffstr_set(&b->bufs[0], ptr0, len0);
	ffstr_set(&b->bufs[1], ptr1, len1);
	b->bufpos = (b->bufpos + len0 + len1) % b->bufsize;

	if (b->bufs[0].len != 0) {
		b->ibuf = 1;
		*buffer = b->bufs[0].ptr;
		return b->bufs[0].len;
	}

	if (b->bufs[1].len != 0) {
		b->ibuf = 2;
		*buffer = b->bufs[1].ptr;
		return b->bufs[1].len;
	}

	if (0 != (r = IDirectSoundCaptureBuffer_Unlock(b->capt_buf, b->bufs[0].ptr, b->bufs[0].len, b->bufs[1].ptr, b->bufs[1].len))) {
		b->errfunc = "IDirectSoundCaptureBuffer_Unlock";
		b->err = r;
		return -FFAUDIO_ERROR;
	}
	return 0;
}

int ffdsound_write(ffaudio_buf *b, const void *data, ffsize len)
{
	int r;
	for (;;) {
		if (0 != (r = dsound_writeonce(b, data, len)))
			return r;

		if (0 != (r = ffdsound_start(b)))
			return r;

		if (b->nonblock)
			return 0;

		Sleep(b->period_ms);
	}
}

int ffdsound_drain(ffaudio_buf *b)
{
	if (b->drained)
		return 1;

	int r;
	for (;;) {
		r = dsound_filled(b);
		if (r < 0)
			return r;
		else if (r == 0 || (ffuint)r > b->last_filled) {
			/* Note: the beginning of the "invalid" data may have been played already
			We should insert silence to avoid that */
			b->drained = 1;
			(void) ffdsound_stop(b);
			return 1;
		}
		b->last_filled = r;

		if (0 != (r = ffdsound_start(b)))
			return r;

		if (b->nonblock)
			return 0;

		Sleep(b->period_ms / 4);
	}
}

int ffdsound_read(ffaudio_buf *b, const void **buffer)
{
	int r;
	for (;;) {
		if (0 != (r = dsound_readonce(b, buffer)))
			return r;

		if (0 != (r = ffdsound_start(b)))
			return r;

		if (b->nonblock)
			return 0;

		Sleep(b->period_ms);
	}
}


const struct ffaudio_interface ffdsound = {
	ffdsound_init,
	ffdsound_uninit,

	ffdsound_dev_alloc,
	ffdsound_dev_free,
	ffdsound_dev_error,
	ffdsound_dev_next,
	ffdsound_dev_info,

	ffdsound_alloc,
	ffdsound_free,
	ffdsound_error,
	ffdsound_open,
	ffdsound_start,
	ffdsound_stop,
	ffdsound_clear,
	ffdsound_write,
	ffdsound_drain,
	ffdsound_read,
	NULL,
};
