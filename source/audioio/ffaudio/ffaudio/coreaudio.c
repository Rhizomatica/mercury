/** ffaudio: CoreAudio wrapper
2020, Simon Zolin
*/

#include <ffaudio/audio.h>
#include <ffbase/ring.h>
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CFString.h>


int ffcoreaudio_init(ffaudio_init_conf *conf)
{
	return 0;
}

void ffcoreaudio_uninit()
{
}


struct ffaudio_dev {
	ffuint mode;
	ffuint idev;
	ffuint ndev;
	AudioObjectID *devs;
	char *name;

	ffuint err;
	char *errmsg;
};

ffaudio_dev* ffcoreaudio_dev_alloc(ffuint mode)
{
	ffaudio_dev *d = ffmem_new(ffaudio_dev);
	if (d == NULL)
		return NULL;
	d->mode = mode;
	return d;
}

void ffcoreaudio_dev_free(ffaudio_dev *d)
{
	if (d == NULL)
		return;
	ffmem_free(d->errmsg);
	ffmem_free(d->devs);
	ffmem_free(d->name);
	ffmem_free(d);
}

static const AudioObjectPropertyAddress prop_dev_list = {
	kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMaster
};
static const AudioObjectPropertyAddress prop_dev_outname = {
	kAudioObjectPropertyName, kAudioDevicePropertyScopeOutput, kAudioObjectPropertyElementMaster
};
static const AudioObjectPropertyAddress prop_dev_inname = {
	kAudioObjectPropertyName, kAudioDevicePropertyScopeInput, kAudioObjectPropertyElementMaster
};
static const AudioObjectPropertyAddress prop_dev_outconf = {
	kAudioDevicePropertyStreamConfiguration, kAudioDevicePropertyScopeOutput, kAudioObjectPropertyElementMaster
};
static const AudioObjectPropertyAddress prop_dev_inconf = {
	kAudioDevicePropertyStreamConfiguration, kAudioDevicePropertyScopeInput, kAudioObjectPropertyElementMaster
};

/** Get device list */
static int coreaudio_dev_list(ffaudio_dev *d)
{
	int rc = FFAUDIO_ERROR;
	OSStatus r;
	ffuint size;
	r = AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &prop_dev_list, 0, NULL, &size);
	if (r != kAudioHardwareNoError)
		return 0;

	if (NULL == (d->devs = ffmem_alloc(size))) {
		d->err = errno;
		return FFAUDIO_ERROR;
	}
	r = AudioObjectGetPropertyData(kAudioObjectSystemObject, &prop_dev_list, 0, NULL, &size, d->devs);
	if (r != kAudioHardwareNoError) {
		d->err = r;
		goto end;
	}
	d->ndev = size / sizeof(AudioObjectID);

	rc = 0;

end:
	if (rc != 0) {
		ffmem_free(d->devs);
		d->devs = NULL;
	}
	return rc;
}

/** Get name of the current device. */
static int coreaudio_dev_name(ffaudio_dev *d)
{
	int rc = FFAUDIO_ERROR;
	const AudioObjectPropertyAddress *prop;
	AudioBufferList *bufs = NULL;
	CFStringRef cfs = NULL;
	OSStatus r;
	ffuint size;

	d->err = 0;

	prop = (d->mode == FFAUDIO_DEV_CAPTURE) ? &prop_dev_inconf : &prop_dev_outconf;
	r = AudioObjectGetPropertyDataSize(d->devs[d->idev], prop, 0, NULL, &size);
	if (r != kAudioHardwareNoError)
		goto end;

	if (NULL == (bufs = ffmem_alloc(size))) {
		d->err = errno;
		rc = FFAUDIO_ERROR;
		goto end;
	}
	r = AudioObjectGetPropertyData(d->devs[d->idev], prop, 0, NULL, &size, bufs);
	if (r != kAudioHardwareNoError)
		goto end;

	ffuint ch = 0;
	for (ffuint i = 0;  i != bufs->mNumberBuffers;  i++) {
		ch |= bufs->mBuffers[i].mNumberChannels;
	}
	if (ch == 0)
		goto end;

	size = sizeof(CFStringRef);
	prop = (d->mode == FFAUDIO_DEV_CAPTURE) ? &prop_dev_inname : &prop_dev_outname;
	r = AudioObjectGetPropertyData(d->devs[d->idev], prop, 0, NULL, &size, &cfs);
	if (r != kAudioHardwareNoError)
		goto end;

	CFIndex len = CFStringGetMaximumSizeForEncoding(CFStringGetLength(cfs), kCFStringEncodingUTF8);
	if (NULL == (d->name = ffmem_alloc(len + 1))) {
		d->err = errno;
		rc = FFAUDIO_ERROR;
		goto end;
	}
	if (!CFStringGetCString(cfs, d->name, len + 1, kCFStringEncodingUTF8))
		goto end;

	rc = 0;

end:
	if (rc == FFAUDIO_ERROR)
		d->err = r;
	ffmem_free(bufs);
	if (rc != 0) {
		ffmem_free(d->name);
		d->name = NULL;
	}
	if (cfs != NULL)
		CFRelease(cfs);
	return rc;
}

int ffcoreaudio_dev_next(ffaudio_dev *d)
{
	if (d->devs == NULL) {
		int r;
		if (0 != (r = coreaudio_dev_list(d)))
			return -r;
	}

	for (;;) {
		ffmem_free(d->name);
		d->name = NULL;

		if (d->idev == d->ndev)
			return 1;

		if (0 != coreaudio_dev_name(d)) {
			d->idev++;
			continue;
		}

		d->idev++;
		return 0;
	}
}

const char* ffcoreaudio_dev_info(ffaudio_dev *d, ffuint i)
{
	switch (i) {
	case FFAUDIO_DEV_ID:
		if (d->idev == 0)
			return NULL;

		return (char*)&d->devs[d->idev - 1];

	case FFAUDIO_DEV_NAME:
		return d->name;
	}
	return NULL;
}

const char* ffcoreaudio_dev_error(ffaudio_dev *d)
{
	ffmem_free(d->errmsg);
	d->errmsg = ffsz_allocfmt("%d (%xu)", d->err, d->err);
	return d->errmsg;
}


struct ffaudio_buf {
	ffuint dev;
	void *aprocid;
	ffring *ring;
	ffuint period_ms;
	ffuint overrun;
	ffuint nonblock;
	ffstr buf_locked;
	ffring_head rhead;

	const char *errfunc;
};

ffaudio_buf* ffcoreaudio_alloc()
{
	ffaudio_buf *b = ffmem_new(ffaudio_buf);
	if (b == NULL)
		return NULL;
	return b;
}

void ffcoreaudio_free(ffaudio_buf *b)
{
	if (b == NULL)
		return;

	ffring_free(b->ring);
	AudioDeviceDestroyIOProcID(b->dev, b->aprocid);
	ffmem_free(b);
}

const char* ffcoreaudio_error(ffaudio_buf *b)
{
	return b->errfunc;
}

static OSStatus coreaudio_ioproc_playback(AudioDeviceID device, const AudioTimeStamp *now,
	const AudioBufferList *indata, const AudioTimeStamp *intime,
	AudioBufferList *outdata, const AudioTimeStamp *outtime,
	void *udata);
static OSStatus coreaudio_ioproc_capture(AudioDeviceID device, const AudioTimeStamp *now,
	const AudioBufferList *indata, const AudioTimeStamp *intime,
	AudioBufferList *outdata, const AudioTimeStamp *outtime,
	void *udata);
static const AudioObjectPropertyAddress prop_odev_fmt = {
	kAudioDevicePropertyStreamFormat, kAudioDevicePropertyScopeOutput, kAudioObjectPropertyElementMaster
};
static const AudioObjectPropertyAddress prop_idev_fmt = {
	kAudioDevicePropertyStreamFormat, kAudioDevicePropertyScopeInput, kAudioObjectPropertyElementMaster
};

/** msec -> bytes:
rate*width*channels*msec/1000 */
static ffuint buffer_msec_to_size(const ffaudio_conf *conf, ffuint msec)
{
	return conf->sample_rate * (conf->format & 0xff) / 8 * conf->channels * msec / 1000;
}

static const AudioObjectPropertyAddress prop_idev_default = {
	kAudioHardwarePropertyDefaultInputDevice, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMaster
};
static const AudioObjectPropertyAddress prop_odev_default = {
	kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMaster
};
static int coreaudio_dev_default(ffuint capture)
{
	AudioObjectID dev;
	ffuint size = sizeof(AudioObjectID);
	const AudioObjectPropertyAddress *a = (capture) ? &prop_idev_default : &prop_odev_default;
	OSStatus r = AudioObjectGetPropertyData(kAudioObjectSystemObject, a, 0, NULL, &size, &dev);
	if (r != 0)
		return -1;
	return dev;
}

int ffcoreaudio_open(ffaudio_buf *b, ffaudio_conf *conf, ffuint flags)
{
	int rc = FFAUDIO_ERROR;
	ffuint capture = (flags & 0x0f) == FFAUDIO_DEV_CAPTURE;
	b->nonblock = !!(flags & FFAUDIO_O_NONBLOCK);

	int dev = -1;
	if (conf->device_id != NULL)
		dev = *(int*)conf->device_id;
	if (dev < 0) {
		dev = coreaudio_dev_default(capture);
		if (dev < 0) {
			b->errfunc = "get default device";
			return FFAUDIO_ERROR;
		}
	}

	AudioStreamBasicDescription asbd = {};
	ffuint size = sizeof(asbd);
	const AudioObjectPropertyAddress *a = (capture) ? &prop_idev_fmt : &prop_odev_fmt;
	if (0 != AudioObjectGetPropertyData(dev, a, 0, NULL, &size, &asbd)) {
		b->errfunc = "AudioStreamBasicDescription";
		return -1;
	}

	int new_format = 0;
	if (conf->format != FFAUDIO_F_FLOAT32) {
		conf->format = FFAUDIO_F_FLOAT32;
		new_format = 1;
	}

	if (conf->sample_rate != asbd.mSampleRate) {
		conf->sample_rate = asbd.mSampleRate;
		new_format = 1;
	}

	if (conf->channels != asbd.mChannelsPerFrame) {
		conf->channels = asbd.mChannelsPerFrame;
		new_format = 1;
	}

	if (new_format)
		return FFAUDIO_EFORMAT;

	void *proc = (capture) ? coreaudio_ioproc_capture : coreaudio_ioproc_playback;
	if (0 != AudioDeviceCreateIOProcID(dev, proc, b, (AudioDeviceIOProcID*)&b->aprocid)
		|| b->aprocid == NULL) {
		b->errfunc = "AudioDeviceCreateIOProcID";
		goto end;
	}

	if (conf->buffer_length_msec == 0)
		conf->buffer_length_msec = 500;
	ffuint bufsize = buffer_msec_to_size(conf, conf->buffer_length_msec);
	if (NULL == (b->ring = ffring_alloc(bufsize, FFRING_1_READER | FFRING_1_WRITER))) {
		b->errfunc = "ffring_alloc";
		goto end;
	}
	b->period_ms = conf->buffer_length_msec / 4;

	b->dev = dev;
	rc = 0;

end:
	if (rc != 0)
		AudioDeviceDestroyIOProcID(b->dev, b->aprocid);
	return rc;
}

int ffcoreaudio_start(ffaudio_buf *b)
{
	if (0 != AudioDeviceStart(b->dev, b->aprocid)) {
		b->errfunc = "AudioDeviceStart";
		return FFAUDIO_ERROR;
	}
	return 0;
}

int ffcoreaudio_stop(ffaudio_buf *b)
{
	if (0 != AudioDeviceStop(b->dev, b->aprocid)) {
		b->errfunc = "AudioDeviceStop";
		return FFAUDIO_ERROR;
	}
	return 0;
}

int ffcoreaudio_clear(ffaudio_buf *b)
{
	ffring_reset(b->ring);
	return 0;
}

static OSStatus coreaudio_ioproc_playback(AudioDeviceID device, const AudioTimeStamp *now,
	const AudioBufferList *indata, const AudioTimeStamp *intime,
	AudioBufferList *outdata, const AudioTimeStamp *outtime,
	void *udata)
{
	char *d = (char*)outdata->mBuffers[0].mData;
	size_t n = outdata->mBuffers[0].mDataByteSize;

	ffaudio_buf *b = udata;
	ffstr s;
	ffring_head h = ffring_read_begin(b->ring, n, &s, NULL);
	if (s.len == 0)
		goto end;
	memcpy(d, s.ptr, s.len);
	d += s.len;
	n -= s.len;
	ffring_read_finish(b->ring, h);

	if (n != 0) {
		h = ffring_read_begin(b->ring, n, &s, NULL);
		if (s.len == 0)
			goto end;
		memcpy(d, s.ptr, s.len);
		d += s.len;
		n -= s.len;
		ffring_read_finish(b->ring, h);
	}

end:
	if (n != 0) {
		memset(d, 0, n);
		b->overrun = 1;
	}

	return 0;
}

static int coreaudio_writeonce(ffaudio_buf *b, const void *data, ffsize len)
{
	ffsize n = ffring_write(b->ring, data, len);
	return n;
}

static OSStatus coreaudio_ioproc_capture(AudioDeviceID device, const AudioTimeStamp *now,
	const AudioBufferList *indata, const AudioTimeStamp *intime,
	AudioBufferList *outdata, const AudioTimeStamp *outtime,
	void *udata)
{
	const float *d = indata->mBuffers[0].mData;
	size_t n = indata->mBuffers[0].mDataByteSize;

	ffaudio_buf *b = udata;
	ffuint r = ffring_write(b->ring, d, n);
	if (r != n) {
		r += ffring_write(b->ring, (char*)d + r, n - r);
		if (r != n)
			b->overrun = 1;
	}
	return 0;
}

static int coreaudio_readonce(ffaudio_buf *b, const void **buffer)
{
	if (b->buf_locked.len != 0) {
		ffring_read_finish(b->ring, b->rhead);
	}

	b->rhead = ffring_read_begin(b->ring, -1, &b->buf_locked, NULL);
	*buffer = b->buf_locked.ptr;
	return b->buf_locked.len;
}

int ffcoreaudio_write(ffaudio_buf *b, const void *data, ffsize len)
{
	for (;;) {
		int r = coreaudio_writeonce(b, data, len);
		if (r != 0)
			return r;

		if (0 != (r = ffcoreaudio_start(b)))
			return r;

		if (b->nonblock)
			return 0;

		usleep(b->period_ms*1000);
	}
}

int ffcoreaudio_drain(ffaudio_buf *b)
{
	int r;
	for (;;) {
		ffstr s;
		ffsize free;
		ffring_write_begin(b->ring, 0, &s, &free);

		if (free == b->ring->cap) {
			(void) ffcoreaudio_stop(b);
			return 1;
		}

		if (0 != (r = ffcoreaudio_start(b)))
			return r;

		if (b->nonblock)
			return 0;

		usleep(b->period_ms*1000);
	}
}

int ffcoreaudio_read(ffaudio_buf *b, const void **buffer)
{
	for (;;) {
		int r = coreaudio_readonce(b, buffer);
		if (r != 0)
			return r;

		if (0 != (r = ffcoreaudio_start(b)))
			return -r;

		if (b->nonblock)
			return 0;

		usleep(b->period_ms*1000);
	}
}


const struct ffaudio_interface ffcoreaudio = {
	ffcoreaudio_init,
	ffcoreaudio_uninit,

	ffcoreaudio_dev_alloc,
	ffcoreaudio_dev_free,
	ffcoreaudio_dev_error,
	ffcoreaudio_dev_next,
	ffcoreaudio_dev_info,

	ffcoreaudio_alloc,
	ffcoreaudio_free,
	ffcoreaudio_error,
	ffcoreaudio_open,
	ffcoreaudio_start,
	ffcoreaudio_stop,
	ffcoreaudio_clear,
	ffcoreaudio_write,
	ffcoreaudio_drain,
	ffcoreaudio_read,
	NULL,
};
