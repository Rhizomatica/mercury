/** ffaudio: CoreAudio for iOS (RemoteIO)

The iOS counterpart of coreaudio.c, exporting the same ffcoreaudio interface so
audioio.c is unchanged.  macOS opens a device by its UID and runs an IOProc on
it; iOS has neither -- no device list and no HAL -- so a stream here is a
RemoteIO AudioUnit on the current audio route (built-in microphone and speaker,
or whatever is plugged in, e.g. a USB radio interface), with the session set up
in coreaudio-ios-session.m.

The streaming side is the same as coreaudio.c's: the AudioUnit callback and the
caller meet in a single-reader single-writer ring, read() and write() block on
it unless the stream is non-blocking, and the format reported is float32 at the
route's rate and channel count, which Mercury converts and resamples.
*/

#include <ffaudio/audio.h>
#include <ffaudio/util.h>
#include <ffbase/ring.h>
#include <AudioToolbox/AudioToolbox.h>
#include <pthread.h>

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* coreaudio-ios-session.m */
extern int ffcoreaudio_ios_session(int activate, double *rate, char *err, size_t cap);
extern int ffcoreaudio_ios_route_name(int capture, char *buf, size_t cap);

/* The largest slice RemoteIO hands a callback.  4096 frames is what the system
 * uses with the screen locked; the default (1156) is not enough there, and a
 * render with a larger slice than allowed fails outright. */
#define IOS_MAX_FRAMES 4096

static char ios_session_err[160];
static double ios_session_rate;
static ffuint ios_session_refs;
static pthread_mutex_t ios_session_lock = PTHREAD_MUTEX_INITIALIZER;

/* audioio.c initialises the backend once per stream, from the capture thread
 * and from the playback thread, so init() runs twice, at the same time.
 * Activating the session twice concurrently makes the system rebuild its
 * default device while the other thread is initialising the I/O unit, and in
 * the simulator that deadlocked: "Initialize: RPC timeout. Apparently
 * deadlocked. Aborting now."  So the session is activated by the first init()
 * and released by the last uninit(). */
int ffcoreaudio_init(ffaudio_init_conf *conf)
{
	int rc = 0;
	pthread_mutex_lock(&ios_session_lock);
	if (ios_session_refs == 0
		&& 0 != ffcoreaudio_ios_session(1, &ios_session_rate, ios_session_err, sizeof(ios_session_err))) {
		conf->error = ios_session_err;
		rc = FFAUDIO_ERROR;
	} else {
		ios_session_refs++;
	}
	pthread_mutex_unlock(&ios_session_lock);
	return rc;
}

void ffcoreaudio_uninit()
{
	pthread_mutex_lock(&ios_session_lock);
	if (ios_session_refs > 0 && --ios_session_refs == 0)
		ffcoreaudio_ios_session(0, NULL, NULL, 0);
	pthread_mutex_unlock(&ios_session_lock);
}


/* One "device" per direction: the current route. */
struct ffaudio_dev {
	ffuint mode;
	ffuint done;
	char name[128];
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
	ffmem_free(d);
}

const char* ffcoreaudio_dev_error(ffaudio_dev *d)
{
	return "no audio route";
}

int ffcoreaudio_dev_next(ffaudio_dev *d)
{
	if (d->done)
		return 1;
	d->done = 1;
	if (0 != ffcoreaudio_ios_route_name(d->mode == FFAUDIO_DEV_CAPTURE, d->name, sizeof(d->name)))
		snprintf(d->name, sizeof(d->name), "%s",
			 (d->mode == FFAUDIO_DEV_CAPTURE) ? "Audio input" : "Audio output");
	return 0;
}

const char* ffcoreaudio_dev_info(ffaudio_dev *d, ffuint i)
{
	switch (i) {
	case FFAUDIO_DEV_ID:
		return "default";
	case FFAUDIO_DEV_NAME:
		return d->name;
	case FFAUDIO_DEV_IS_DEFAULT:
		return "1";
	}
	return NULL;
}


struct ffaudio_buf {
	ffuint capture;
	ffuint started;
	ffring *ring;
	ffuint period_ms;
	ffuint overrun;
	ffuint nonblock;
	ffstr buf_locked;
	ffring_head rhead;

	const char *errfunc;
	char errbuf[96];
};

/* ONE RemoteIO unit serves both directions.  Two units in one process -- one
 * for capture, one for playback -- each initialised from its own thread, is
 * not how RemoteIO is meant to be used: they contend for the same hardware,
 * and in the simulator the second AudioUnitInitialize blocked on the audio
 * server until AudioToolbox aborted the process (_ReportRPCTimeout, 15 s).
 *
 * The first stream to open, of either direction, sets the unit up for both,
 * with input and output enabled and both formats fixed by the route, and
 * initialises it once.  Each stream then registers itself; the callbacks serve
 * whichever streams are registered and started, and the unit runs while either
 * is started.  ios_lock guards everything here except the two stream pointers,
 * which the callbacks read without it. */
static struct {
	AudioComponentInstance unit;
	double rate;
	ffuint cap_ch, play_ch;          /* 0: the route has no such side */
	ffuint running;
	ffaudio_buf *_Atomic cap;
	ffaudio_buf *_Atomic play;
	float *scratch;                  /* capture renders here: nothing may
	                                  * allocate on the audio thread */
	char errbuf[96];
} io;
static pthread_mutex_t ios_lock = PTHREAD_MUTEX_INITIALIZER;

/* Format "func: 'four' (-10851)", as coreaudio.c does. */
static void ios_fmt_err(char *buf, size_t cap, const char *func, OSStatus st)
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
		snprintf(buf, cap, "%s: '%c%c%c%c' (%d)", func, c[0], c[1], c[2], c[3], (int)st);
	else
		snprintf(buf, cap, "%s: (%d)", func, (int)st);
}

static void ios_err(ffaudio_buf *b, const char *func, OSStatus st)
{
	ios_fmt_err(b->errbuf, sizeof(b->errbuf), func, st);
	b->errfunc = b->errbuf;
}

ffaudio_buf* ffcoreaudio_alloc()
{
	return ffmem_new(ffaudio_buf);
}

const char* ffcoreaudio_error(ffaudio_buf *b)
{
	return b->errfunc;
}

static OSStatus ios_capture_cb(void *udata, AudioUnitRenderActionFlags *flags,
	const AudioTimeStamp *ts, UInt32 bus, UInt32 frames, AudioBufferList *unused)
{
	ffaudio_buf *b = io.cap;
	if (b == NULL || !b->started)
		return noErr;
	if (frames > IOS_MAX_FRAMES) {
		b->overrun = 1;
		return noErr;
	}
	ffuint bpf = 4 * io.cap_ch;
	AudioBufferList abl;
	abl.mNumberBuffers = 1;
	abl.mBuffers[0].mNumberChannels = io.cap_ch;
	abl.mBuffers[0].mDataByteSize = frames * bpf;
	abl.mBuffers[0].mData = io.scratch;
	OSStatus st = AudioUnitRender(io.unit, flags, ts, bus, frames, &abl);
	if (st != noErr)
		return st;

	const char *d = abl.mBuffers[0].mData;
	size_t n = abl.mBuffers[0].mDataByteSize;
	ffuint r = ffring_write(b->ring, d, n);
	if (r != n) {
		r += ffring_write(b->ring, d + r, n - r);
		if (r != n)
			b->overrun = 1;
	}
	return noErr;
}

static OSStatus ios_playback_cb(void *udata, AudioUnitRenderActionFlags *flags,
	const AudioTimeStamp *ts, UInt32 bus, UInt32 frames, AudioBufferList *outdata)
{
	char *d = (char*)outdata->mBuffers[0].mData;
	size_t n = outdata->mBuffers[0].mDataByteSize;

	ffaudio_buf *b = io.play;
	if (b == NULL || !b->started)
		goto end;

	ffstr s;
	ffring_head h = ffring_read_begin(b->ring, n, &s, NULL);
	if (s.len == 0)
		goto underrun;
	memcpy(d, s.ptr, s.len);
	d += s.len;
	n -= s.len;
	ffring_read_finish(b->ring, h);

	if (n != 0) {
		h = ffring_read_begin(b->ring, n, &s, NULL);
		if (s.len == 0)
			goto underrun;
		memcpy(d, s.ptr, s.len);
		d += s.len;
		n -= s.len;
		ffring_read_finish(b->ring, h);
	}
	return noErr;

underrun:
	b->overrun = 1;
end:
	memset(d, 0, n);
	return noErr;
}

/* RemoteIO's elements: 1 is the input (microphone) side, 0 the output
 * (speaker) side.  The route's format is on the outer scopes -- input scope of
 * element 1, output scope of element 0 -- and ours on the inner ones. */
#define IOS_IN_BUS  1
#define IOS_OUT_BUS 0

static AudioStreamBasicDescription ios_client_format(double rate, ffuint channels)
{
	AudioStreamBasicDescription fmt = {};
	fmt.mSampleRate = rate;
	fmt.mFormatID = kAudioFormatLinearPCM;
	fmt.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
	fmt.mChannelsPerFrame = channels;
	fmt.mBitsPerChannel = 32;
	fmt.mFramesPerPacket = 1;
	fmt.mBytesPerFrame = 4 * channels;
	fmt.mBytesPerPacket = fmt.mBytesPerFrame;
	return fmt;
}

/* Tear the unit down.  Called with ios_lock held, once neither stream is
 * registered. */
static void ios_unit_dispose(void)
{
	if (io.unit != NULL) {
		AudioOutputUnitStop(io.unit);
		AudioUnitUninitialize(io.unit);
		AudioComponentInstanceDispose(io.unit);
	}
	ffmem_free(io.scratch);
	io.unit = NULL;
	io.scratch = NULL;
	io.running = 0;
	io.rate = 0;
	io.cap_ch = io.play_ch = 0;
}

/* Create and initialise the shared unit for both directions.  Called with
 * ios_lock held.  Returns 0, or -1 with the reason in io.errbuf. */
static int ios_unit_setup(void)
{
	OSStatus st;
	AudioComponentDescription desc = {
		kAudioUnitType_Output, kAudioUnitSubType_RemoteIO,
		kAudioUnitManufacturer_Apple, 0, 0
	};
	AudioComponent comp = AudioComponentFindNext(NULL, &desc);
	if (comp == NULL) {
		snprintf(io.errbuf, sizeof(io.errbuf), "RemoteIO not available");
		return -1;
	}
	if (0 != (st = AudioComponentInstanceNew(comp, &io.unit))) {
		ios_fmt_err(io.errbuf, sizeof(io.errbuf), "AudioComponentInstanceNew", st);
		io.unit = NULL;
		return -1;
	}

	/* The route's channels on each side.  The rate may read 0 before the unit
	 * is initialised; the session's rate is the same route's. */
	AudioStreamBasicDescription hw_in = {}, hw_out = {};
	UInt32 size = sizeof(hw_in);
	AudioUnitGetProperty(io.unit, kAudioUnitProperty_StreamFormat,
		kAudioUnitScope_Input, IOS_IN_BUS, &hw_in, &size);
	size = sizeof(hw_out);
	AudioUnitGetProperty(io.unit, kAudioUnitProperty_StreamFormat,
		kAudioUnitScope_Output, IOS_OUT_BUS, &hw_out, &size);
	io.rate = hw_out.mSampleRate > 0 ? hw_out.mSampleRate
		: (hw_in.mSampleRate > 0 ? hw_in.mSampleRate : ios_session_rate);
	io.cap_ch = hw_in.mChannelsPerFrame;
	io.play_ch = hw_out.mChannelsPerFrame > 0 ? hw_out.mChannelsPerFrame : 2;
	if (io.rate <= 0) {
		snprintf(io.errbuf, sizeof(io.errbuf), "no sample rate (audio session not active)");
		goto fail;
	}

	UInt32 in_on = io.cap_ch > 0, out_on = 1;
	AudioUnitSetProperty(io.unit, kAudioOutputUnitProperty_EnableIO,
		kAudioUnitScope_Input, IOS_IN_BUS, &in_on, sizeof(UInt32));
	AudioUnitSetProperty(io.unit, kAudioOutputUnitProperty_EnableIO,
		kAudioUnitScope_Output, IOS_OUT_BUS, &out_on, sizeof(UInt32));

	AudioStreamBasicDescription fmt = ios_client_format(io.rate, io.play_ch);
	if (0 != (st = AudioUnitSetProperty(io.unit, kAudioUnitProperty_StreamFormat,
			kAudioUnitScope_Input, IOS_OUT_BUS, &fmt, sizeof(fmt)))) {
		ios_fmt_err(io.errbuf, sizeof(io.errbuf), "AudioUnitSetProperty(playback format)", st);
		goto fail;
	}
	AURenderCallbackStruct pcb = { ios_playback_cb, NULL };
	if (0 != (st = AudioUnitSetProperty(io.unit, kAudioUnitProperty_SetRenderCallback,
			kAudioUnitScope_Input, IOS_OUT_BUS, &pcb, sizeof(pcb)))) {
		ios_fmt_err(io.errbuf, sizeof(io.errbuf), "AudioUnitSetProperty(render callback)", st);
		goto fail;
	}

	if (io.cap_ch > 0) {
		fmt = ios_client_format(io.rate, io.cap_ch);
		if (0 != (st = AudioUnitSetProperty(io.unit, kAudioUnitProperty_StreamFormat,
				kAudioUnitScope_Output, IOS_IN_BUS, &fmt, sizeof(fmt)))) {
			ios_fmt_err(io.errbuf, sizeof(io.errbuf), "AudioUnitSetProperty(capture format)", st);
			goto fail;
		}
		AURenderCallbackStruct ccb = { ios_capture_cb, NULL };
		if (0 != (st = AudioUnitSetProperty(io.unit, kAudioOutputUnitProperty_SetInputCallback,
				kAudioUnitScope_Global, IOS_IN_BUS, &ccb, sizeof(ccb)))) {
			ios_fmt_err(io.errbuf, sizeof(io.errbuf), "AudioUnitSetProperty(input callback)", st);
			goto fail;
		}
		if (NULL == (io.scratch = ffmem_alloc((ffsize)IOS_MAX_FRAMES * 4 * io.cap_ch))) {
			snprintf(io.errbuf, sizeof(io.errbuf), "ffmem_alloc");
			goto fail;
		}
	}

	UInt32 maxf = IOS_MAX_FRAMES;
	AudioUnitSetProperty(io.unit, kAudioUnitProperty_MaximumFramesPerSlice,
		kAudioUnitScope_Global, 0, &maxf, sizeof(maxf));

	if (0 != (st = AudioUnitInitialize(io.unit))) {
		ios_fmt_err(io.errbuf, sizeof(io.errbuf), "AudioUnitInitialize", st);
		goto fail;
	}
	return 0;

fail:
	ios_unit_dispose();
	return -1;
}

int ffcoreaudio_open(ffaudio_buf *b, ffaudio_conf *conf, ffuint flags)
{
	ffuint capture = (flags & 0x0f) == FFAUDIO_DEV_CAPTURE;
	b->capture = capture;
	b->nonblock = !!(flags & FFAUDIO_O_NONBLOCK);

	/* There is one device per direction, the current route.  A configuration
	 * naming any other device (e.g. a macOS UID carried over) is refused, as
	 * coreaudio.c refuses a UID that is not present, rather than quietly
	 * opening the route instead. */
	if (conf->device_id != NULL && conf->device_id[0] != '\0'
		&& strcmp(conf->device_id, "default") != 0) {
		b->errfunc = "device not found: iOS uses the current audio route (device \"default\")";
		return FFAUDIO_ERROR;
	}

	pthread_mutex_lock(&ios_lock);
	int rc = FFAUDIO_ERROR;
	if (io.unit == NULL && 0 != ios_unit_setup()) {
		snprintf(b->errbuf, sizeof(b->errbuf), "%s", io.errbuf);
		b->errfunc = b->errbuf;
		goto end;
	}
	ffuint channels = capture ? io.cap_ch : io.play_ch;
	if (channels == 0) {
		b->errfunc = "the audio route has no input";
		goto end;
	}
	if ((capture ? io.cap : io.play) != NULL) {
		b->errfunc = "already open: one stream per direction";
		goto end;
	}

	int new_format = 0;
	if (conf->format != FFAUDIO_F_FLOAT32) {
		conf->format = FFAUDIO_F_FLOAT32;
		new_format = 1;
	}
	if (conf->sample_rate != (ffuint)io.rate) {
		conf->sample_rate = (ffuint)io.rate;
		new_format = 1;
	}
	if (conf->channels != channels) {
		conf->channels = channels;
		new_format = 1;
	}
	if (new_format) {
		rc = FFAUDIO_EFORMAT;
		goto end;
	}

	if (conf->buffer_length_msec == 0)
		conf->buffer_length_msec = 500;
	ffuint bufsize = _ffau_buf_msec_to_size(conf, conf->buffer_length_msec);
	if (b->ring == NULL
		&& NULL == (b->ring = ffring_alloc(bufsize, FFRING_1_READER | FFRING_1_WRITER))) {
		b->errfunc = "ffring_alloc";
		goto end;
	}
	b->period_ms = conf->buffer_length_msec / 4;

	if (capture)
		io.cap = b;
	else
		io.play = b;
	rc = 0;

end:
	/* A first open that only asked for the format (EFORMAT) or failed leaves no
	 * stream registered; keep the unit only while someone uses it. */
	if (rc != 0 && io.cap == NULL && io.play == NULL)
		ios_unit_dispose();
	pthread_mutex_unlock(&ios_lock);
	return rc;
}

void ffcoreaudio_free(ffaudio_buf *b)
{
	if (b == NULL)
		return;

	pthread_mutex_lock(&ios_lock);
	if (io.cap == b || io.play == b) {
		/* The callbacks read the stream pointers without the lock, so the unit
		 * is stopped -- AudioOutputUnitStop waits for a callback in flight --
		 * before this stream is unhooked and its ring freed, then started
		 * again for the other stream if that one is running. */
		if (io.running) {
			AudioOutputUnitStop(io.unit);
			io.running = 0;
		}
		b->started = 0;
		if (io.cap == b)
			io.cap = NULL;
		else
			io.play = NULL;

		ffaudio_buf *other = io.cap != NULL ? io.cap : io.play;
		if (other == NULL)
			ios_unit_dispose();
		else if (other->started && 0 == AudioOutputUnitStart(io.unit))
			io.running = 1;
	}
	pthread_mutex_unlock(&ios_lock);

	ffring_free(b->ring);
	ffmem_free(b);
}

int ffcoreaudio_start(ffaudio_buf *b)
{
	if (b->started)
		return 0;
	int rc = 0;
	pthread_mutex_lock(&ios_lock);
	b->started = 1;
	if (!io.running) {
		OSStatus st = AudioOutputUnitStart(io.unit);
		if (st != 0) {
			ios_err(b, "AudioOutputUnitStart", st);
			b->started = 0;
			rc = FFAUDIO_ERROR;
		} else {
			io.running = 1;
		}
	}
	pthread_mutex_unlock(&ios_lock);
	return rc;
}

/* Stop this stream.  The unit keeps running for the other direction if that
 * one is started: stopping playback after a transmission must not stop the
 * receiver. */
int ffcoreaudio_stop(ffaudio_buf *b)
{
	if (!b->started)
		return 0;
	int rc = 0;
	pthread_mutex_lock(&ios_lock);
	b->started = 0;
	ffaudio_buf *other = (b == io.cap) ? io.play : io.cap;
	if (io.running && (other == NULL || !other->started)) {
		OSStatus st = AudioOutputUnitStop(io.unit);
		if (st != 0) {
			ios_err(b, "AudioOutputUnitStop", st);
			rc = FFAUDIO_ERROR;
		} else {
			io.running = 0;
		}
	}
	pthread_mutex_unlock(&ios_lock);
	return rc;
}

int ffcoreaudio_clear(ffaudio_buf *b)
{
	ffring_reset(b->ring);
	return 0;
}

static int ios_readonce(ffaudio_buf *b, const void **buffer)
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
		int r = ffring_write(b->ring, data, len);
		if (r != 0)
			return r;

		if (0 != (r = ffcoreaudio_start(b)))
			return -r;

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
			return -r;

		if (b->nonblock)
			return 0;

		usleep(b->period_ms*1000);
	}
}

int ffcoreaudio_read(ffaudio_buf *b, const void **buffer)
{
	for (;;) {
		int r = ios_readonce(b, buffer);
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
