/** ffaudio: CoreAudio wrapper
2020, Simon Zolin
*/

/* macOS 12 renamed kAudioObjectPropertyElementMaster to ...Main and deprecated
 * the old spelling; the value is the same.  Tahoe warns on every one of the ten
 * uses in this file, which buries anything else the build has to say -- and
 * this is the file people are asked to build when a sound-card bug is being
 * chased.  Use the modern name, and fall back to the old one on SDKs that
 * predate it.
 *
 * This has to come FIRST, before ffaudio/ffbase.  Including those first leaves
 * MAC_OS_VERSION_12_0 undefined -- verified by compiling the same test with and
 * without them -- so the guard silently took the fallback branch and the file
 * went on using the deprecated name while looking as though it did not.  An
 * undefined macro is 0 in #if, which makes that failure quiet in exactly the
 * wrong direction. */
#include <AvailabilityMacros.h>
#if !defined(MAC_OS_VERSION_12_0) \
	|| (MAC_OS_X_VERSION_MAX_ALLOWED < MAC_OS_VERSION_12_0)
	#define kAudioObjectPropertyElementMain kAudioObjectPropertyElementMaster
#endif

#include <ffaudio/audio.h>
#include <ffaudio/util.h>
#include <ffbase/ring.h>
#include <CoreAudio/CoreAudio.h>

#include <CoreFoundation/CFString.h>
#include <stdlib.h>
#include <stdio.h>


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
	char id_str[256];
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
	kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain
};
static const AudioObjectPropertyAddress prop_dev_uid = {
	kAudioDevicePropertyDeviceUID, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain
};
static const AudioObjectPropertyAddress prop_dev_outname = {
	kAudioObjectPropertyName, kAudioDevicePropertyScopeOutput, kAudioObjectPropertyElementMain
};
static const AudioObjectPropertyAddress prop_dev_inname = {
	kAudioObjectPropertyName, kAudioDevicePropertyScopeInput, kAudioObjectPropertyElementMain
};
static const AudioObjectPropertyAddress prop_dev_outconf = {
	kAudioDevicePropertyStreamConfiguration, kAudioDevicePropertyScopeOutput, kAudioObjectPropertyElementMain
};
static const AudioObjectPropertyAddress prop_dev_inconf = {
	kAudioDevicePropertyStreamConfiguration, kAudioDevicePropertyScopeInput, kAudioObjectPropertyElementMain
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

/* A device's persistent unique id.
 *
 * AudioDeviceID is an enumeration index, not an identity: unplug a USB
 * interface and plug it back and it gets a different one, and after a reboot
 * the numbering can differ again.  Anything stored in a config file therefore
 * has to be the UID, which CoreAudio guarantees is stable for the device and
 * unique between devices -- including between two units of the same model,
 * which share a display name and defeat name matching (see issue #189 and
 * ui_devices_disambiguate).
 *
 * Returns 0 on success and fills buf with a NUL-terminated UTF-8 UID. */
static int coreaudio_dev_uid(AudioDeviceID dev, char *buf, size_t cap)
{
	CFStringRef cfs = NULL;
	ffuint size = sizeof(cfs);
	if (0 != AudioObjectGetPropertyData(dev, &prop_dev_uid, 0, NULL, &size, &cfs)
		|| cfs == NULL)
		return -1;
	Boolean ok = CFStringGetCString(cfs, buf, (CFIndex)cap, kCFStringEncodingUTF8);
	CFRelease(cfs);
	return ok ? 0 : -1;
}

/* Defined below, next to the open path that is its main user. */
static int coreaudio_dev_default(ffuint capture);

const char* ffcoreaudio_dev_info(ffaudio_dev *d, ffuint i)
{
	switch (i) {
	case FFAUDIO_DEV_ID:
		if (d->idev == 0)
			return NULL;

		/* Report the UID, not the index: this string is what the UI stores
		 * and hands back on the next run, possibly after a reboot. */
		if (0 == coreaudio_dev_uid(d->devs[d->idev - 1], d->id_str, sizeof(d->id_str)))
			return d->id_str;
		snprintf(d->id_str, sizeof(d->id_str), "%u", (unsigned)d->devs[d->idev - 1]);
		return d->id_str;

	case FFAUDIO_DEV_NAME:
		return d->name;

	case FFAUDIO_DEV_IS_DEFAULT: {
		/* Which device the system would pick if the operator picked nothing.
		 * The UI needs this to preselect sensibly on a first run, and an
		 * operator reading -z needs to know which line is the one Mercury
		 * would open by default -- on this VM the two VoodooHDA devices are
		 * both called "Unknown Codec ... (N/A)", so the name does not say. */
		if (d->idev == 0)
			return NULL;
		int def = coreaudio_dev_default(d->mode == FFAUDIO_DEV_CAPTURE);
		return (def >= 0 && (AudioObjectID)def == d->devs[d->idev - 1])
			? "1" : NULL;
	}
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
	/* CoreAudio reports every failure as an OSStatus, and the reason lives in
	 * that code -- device gone, format refused, permission, hardware not
	 * running.  Recording only the function name (as this did) turns a
	 * diagnosable fault into "AudioDeviceStart failed" with nothing to act on;
	 * see issue #254, where that is exactly what happened. */
	char errbuf[96];
};

/* Format "func: 'four' (-10851)".  Most CoreAudio statuses are four-character
 * codes ('nope', '!dat', 'stop'); the numeric form is kept for the ones that
 * are not printable. */
static void coreaudio_err(ffaudio_buf *b, const char *func, OSStatus st)
{
	unsigned char c[4] = {
		(unsigned char)(st >> 24), (unsigned char)(st >> 16),
		(unsigned char)(st >> 8),  (unsigned char)st,
	};
	int printable = 1;
	for (int i = 0; i != 4; i++) {
		if (c[i] < 0x20 || c[i] > 0x7e)
			printable = 0;
	}
	if (printable)
		snprintf(b->errbuf, sizeof(b->errbuf), "%s: '%c%c%c%c' (%d)",
			 func, c[0], c[1], c[2], c[3], (int)st);
	else
		snprintf(b->errbuf, sizeof(b->errbuf), "%s: (%d)", func, (int)st);
	b->errfunc = b->errbuf;
}

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

	/* Order matters: the IOProc writes into b->ring from CoreAudio's own
	 * thread, so it has to be stopped and destroyed BEFORE the ring is freed.
	 * Freeing first leaves a live callback writing into released memory --
	 * rare, timing-dependent, and exactly the kind of fault that shows up as
	 * an unrelated crash much later.  Stop first: destroying a running IOProc
	 * is not defined to be safe. */
	if (b->aprocid != NULL) {
		AudioDeviceStop(b->dev, (AudioDeviceIOProcID)b->aprocid);
		AudioDeviceDestroyIOProcID(b->dev, (AudioDeviceIOProcID)b->aprocid);
		b->aprocid = NULL;
	}
	ffring_free(b->ring);
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
	kAudioDevicePropertyStreamFormat, kAudioDevicePropertyScopeOutput, kAudioObjectPropertyElementMain
};
static const AudioObjectPropertyAddress prop_idev_fmt = {
	kAudioDevicePropertyStreamFormat, kAudioDevicePropertyScopeInput, kAudioObjectPropertyElementMain
};

static const AudioObjectPropertyAddress prop_idev_default = {
	kAudioHardwarePropertyDefaultInputDevice, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain
};
static const AudioObjectPropertyAddress prop_odev_default = {
	kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain
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

/* Find the device currently carrying this UID.  Returns -1 if no device does,
 * which is the honest answer when the interface is unplugged -- better than
 * opening whatever now holds some remembered index. */
static int coreaudio_dev_by_uid(const char *uid)
{
	AudioDeviceID *devs = NULL;
	ffuint size = 0;
	int found = -1;

	if (uid == NULL || uid[0] == '\0')
		return -1;
	if (0 != AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &prop_dev_list,
						0, NULL, &size) || size == 0)
		return -1;
	if (NULL == (devs = (AudioDeviceID *)ffmem_alloc(size)))
		return -1;
	if (0 != AudioObjectGetPropertyData(kAudioObjectSystemObject, &prop_dev_list,
					    0, NULL, &size, devs)) {
		ffmem_free(devs);
		return -1;
	}

	ffuint n = size / sizeof(AudioDeviceID);
	char buf[256];
	for (ffuint i = 0; i != n; i++) {
		if (0 == coreaudio_dev_uid(devs[i], buf, sizeof(buf))
			&& 0 == strcmp(buf, uid)) {
			found = (int)devs[i];
			break;
		}
	}
	ffmem_free(devs);
	return found;
}

int ffcoreaudio_open(ffaudio_buf *b, ffaudio_conf *conf, ffuint flags)
{
	int rc = FFAUDIO_ERROR;
	ffuint capture = (flags & 0x0f) == FFAUDIO_DEV_CAPTURE;
	b->nonblock = !!(flags & FFAUDIO_O_NONBLOCK);

	/* Devices are addressed by UID.  An AudioDeviceID is an enumeration
	 * index, not an identity -- replug the interface or reboot and it moves,
	 * which is how a stored index ends up addressing nothing (issue #254).
	 * kAudioDevicePropertyDeviceUID is stable for the device and distinct
	 * between two units of the same model, so it is what enumeration reports
	 * and what a config holds; resolve it fresh on every open. */
	int dev = -1;
	if (conf->device_id != NULL && conf->device_id[0] != '\0') {
		dev = coreaudio_dev_by_uid(conf->device_id);
		if (dev < 0) {
			/* An explicitly chosen device that is not present is an error, not
			 * an invitation to open something else.  Falling back to the
			 * default here would quietly bind the built-in microphone and the
			 * modem would sit there hearing nothing -- far worse to diagnose
			 * than a refusal. */
			b->errfunc = "device UID not found (run -z to list devices)";
			return FFAUDIO_ERROR;
		}
	} else {
		dev = coreaudio_dev_default(capture);
		if (dev < 0) {
			b->errfunc = "get default device";
			return FFAUDIO_ERROR;
		}
	}

	AudioStreamBasicDescription asbd = {};
	ffuint size = sizeof(asbd);
	const AudioObjectPropertyAddress *a = (capture) ? &prop_idev_fmt : &prop_odev_fmt;
	OSStatus st;
	if (0 != (st = AudioObjectGetPropertyData(dev, a, 0, NULL, &size, &asbd))) {
		coreaudio_err(b, "AudioStreamBasicDescription", st);
		return FFAUDIO_ERROR;
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
	b->aprocid = NULL;
	if (0 != (st = AudioDeviceCreateIOProcID(dev, proc, b, (AudioDeviceIOProcID*)&b->aprocid))
		|| b->aprocid == NULL) {
		coreaudio_err(b, "AudioDeviceCreateIOProcID", st);
		goto end;
	}

	if (conf->buffer_length_msec == 0)
		conf->buffer_length_msec = 500;
	ffuint bufsize = _ffau_buf_msec_to_size(conf, conf->buffer_length_msec);
	if (NULL == (b->ring = ffring_alloc(bufsize, FFRING_1_READER | FFRING_1_WRITER))) {
		b->errfunc = "ffring_alloc";
		goto end;
	}
	b->period_ms = conf->buffer_length_msec / 4;

	b->dev = dev;
	rc = 0;

end:
	if (rc != 0) {
		/* Destroy the proc on the device it was CREATED on.  b->dev is only
		 * assigned on the success path above, so it still holds 0 (first open)
		 * or the previous device (a reopen) -- destroying against it leaves the
		 * new proc registered on `dev` for the lifetime of the process, one
		 * leaked proc per failed open.  A reopen loop retrying every 200 ms,
		 * as in issue #254, leaks them at that rate. */
		if (b->aprocid != NULL) {
			/* Only drop the handle if the proc is really gone.  Clearing it
			 * unconditionally discards the one reference we have to a proc
			 * that is still registered on the device, so nothing can ever
			 * clean it up -- the leak this block exists to prevent, arrived
			 * at from the other side.  Keep it and let ffcoreaudio_free()
			 * try again; b->dev is set below only on success, so record the
			 * device the proc actually belongs to. */
			if (0 == AudioDeviceDestroyIOProcID(dev, (AudioDeviceIOProcID)b->aprocid))
				b->aprocid = NULL;
			else
				b->dev = dev;
		}
	}
	return rc;
}

int ffcoreaudio_start(ffaudio_buf *b)
{
	OSStatus st;
	if (0 != (st = AudioDeviceStart(b->dev, b->aprocid))) {
		coreaudio_err(b, "AudioDeviceStart", st);
		return FFAUDIO_ERROR;
	}
	return 0;
}

int ffcoreaudio_stop(ffaudio_buf *b)
{
	OSStatus st;
	if (0 != (st = AudioDeviceStop(b->dev, b->aprocid))) {
		coreaudio_err(b, "AudioDeviceStop", st);
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
