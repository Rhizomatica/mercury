/** ffaudio: tester
2020, Simon Zolin
*/

#include <stdint.h>
#include <pthread.h>
#include <ffaudio/audio.h>
#include <ffbase/args.h>
#include <ffbase/stringz.h>
#include "std.h"
#include "audioio.h"
#ifdef FF_LINUX
#include <time.h>
#endif

int audio_subsystem;

struct conf {
	const char *cmd;
	ffaudio_conf buf;
	uint8_t flags;
	uint8_t exclusive;
	uint8_t hwdev;
	uint8_t loopback;
	uint8_t nonblock;
	uint8_t wav;
};


static inline void ffthread_sleep(ffuint msec)
{
#ifdef FF_WIN
	Sleep(msec);
#else
	struct timespec ts = {
		.tv_sec = msec / 1000,
		.tv_nsec = (msec % 1000) * 1000000,
	};
	nanosleep(&ts, NULL);
#endif
}


void *radio_playback_thread(void *device_ptr)
{
    ffaudio_interface *audio;
	struct conf conf = {};
	conf.buf.app_name = "mercury_playback";
	conf.buf.buffer_length_msec = 40;
	conf.buf.format = FFAUDIO_F_INT16;
	conf.buf.sample_rate = 48000;
	conf.buf.channels = 2;
	conf.buf.device_id = device_ptr;


#ifdef _WIN32
    audio = (ffaudio_interface *) &ffdsound;
#else
    audio = (ffaudio_interface *) &ffalsa;
#endif

    conf.flags = FFAUDIO_PLAYBACK;
	ffaudio_init_conf aconf = {};
	aconf.app_name = "mercury_playback";
	xieq(0, audio->init(&aconf));

    // playback code...
	int r;
	ffaudio_buf *b;

	b = audio->alloc();
	x(b != NULL);

	ffstdout_fmt("ffaudio.open...");
    ffaudio_conf *cfg = &conf.buf;
	r = audio->open(b, cfg, conf.flags);
	if (r == FFAUDIO_EFORMAT)
		r = audio->open(b, cfg, conf.flags);
	if (r != 0)
		fflog("ffaudio.open: %d: %s", r, audio->error(b));
	xieq(0, r);

	fflog(" %d/%d/%d %dms"
		, cfg->format, cfg->sample_rate, cfg->channels
		, cfg->buffer_length_msec);

	ffuint frame_size = cfg->channels * (cfg->format & 0xff) / 8;
	ffuint sec_bytes = cfg->sample_rate * cfg->channels * (cfg->format & 0xff) / 8;
	ffuint cap = sec_bytes;
	void *buffer = ffmem_alloc(cap);
	ffstr data;
	ffstr_set(&data, buffer, 0);

	ffuint total = 0;
	ffuint total_written = 0;

	for (;;) {
		ffssize n = ffstdin_read(data.ptr, cap - data.len);
		if (n == 0)
			break;
		x(n >= 0);
		data.len += n;
		total += n;

		while (data.len >= frame_size)
        {
			ffstdout_fmt("ffaudio.write...");
			r = audio->write(b, data.ptr, data.len);

			if (r == -FFAUDIO_ESYNC) {
				fflog("detected underrun");
				continue;
			}
			if (r < 0)
				fflog("ffaudio.write: %s", audio->error(b));
			else
				fflog(" %dms", r * 1000 / sec_bytes);
			x(r >= 0);
			ffstr_shift(&data, r);
			total_written += r;
		}

		ffmem_move(buffer, data.ptr, data.len);
		data.ptr = buffer + data.len;
	}

	// noop
	r = audio->start(b);
	if (r != 0)
		fflog("ffaudio.start: %s", audio->error(b));
	xieq(0, r);

	fflog("ffaudio.drain...");
	for (;;) {
		r = audio->drain(b);
		if (r < 0)
			fflog("ffaudio.drain: %s", audio->error(b));
		if (r != 0)
			break;
	}
	x(r == 1);

	fflog("ffaudio.drain #2...");
	r = audio->drain(b);
	if (r < 0)
		fflog("ffaudio.drain: %s", audio->error(b));
	x(r == 1);

	r = audio->stop(b);
	if (r != 0)
		fflog("ffaudio.stop: %s", audio->error(b));
	xieq(0, r);

	r = audio->clear(b);
	if (r != 0)
		fflog("ffaudio.clear: %s", audio->error(b));
	xieq(0, r);

	audio->free(b);
	ffmem_free(buffer);
	fflog("play done");

	audio->uninit();

    return NULL;
}

void *radio_capture_thread(void *device_ptr)
{
    ffaudio_interface *audio;
	struct conf conf = {};
	conf.buf.app_name = "mercury_capture";
	conf.buf.buffer_length_msec = 40;
	conf.buf.format = FFAUDIO_F_INT16;
	conf.buf.sample_rate = 48000;
	conf.buf.channels = 2;
	conf.buf.device_id = device_ptr;


#if defined(_WIN32)
    if (audio_subsystem == AUDIO_SUBSYSTEM_WASAPI)
        audio = (ffaudio_interface *) &ffwasapi;
    if (audio_subsystem == AUDIO_SUBSYSTEM_DSOUND)
        audio = (ffaudio_interface *) &ffdsound;
#elif defined(__linux__)
    if (audio_subsystem == AUDIO_SUBSYSTEM_ALSA)
        audio = (ffaudio_interface *) &ffalsa;
    if (audio_subsystem == AUDIO_SUBSYSTEM_PULSE)
        audio = (ffaudio_interface *) &ffpulse;
#elif defined(__FREEBSD__)
    if (audio_subsystem == AUDIO_SUBSYSTEM_OSS)
        audio = (ffaudio_interface *) &ffoss;
#elif defined(__APPLE__)
    if (audio_subsystem == AUDIO_SUBSYSTEM_COREAUDIO)
        audio = (ffaudio_interface *) &ffcoreaudio;
#endif

    conf.flags = FFAUDIO_CAPTURE;
	ffaudio_init_conf aconf = {};
	aconf.app_name = "mercury_capture";
	xieq(0, audio->init(&aconf));

    // capture code
	int r;
	ffaudio_buf *b;
	b = audio->alloc();
	x(b != NULL);

	ffstdout_fmt("ffaudio.open...");
    ffaudio_conf *cfg = &conf.buf;
	r = audio->open(b, cfg, conf.flags);
	if (r == FFAUDIO_EFORMAT) {
		ffstdout_fmt(" reopening...");
		r = audio->open(b, cfg, conf.flags);
	}
	if (r != 0)
		fflog("ffaudio.open: %d: %s", r, audio->error(b));
	xieq(0, r);
	fflog(" %d/%d/%d %dms"
		, cfg->format, cfg->sample_rate, cfg->channels
		, cfg->buffer_length_msec);

	ffuint msec_bytes = cfg->sample_rate * cfg->channels * (cfg->format & 0xff) / 8 / 1000;
	ffstr data = {};

	for (;;) {
		ffstdout_fmt("ffaudio.read...");
		r = audio->read(b, (const void**)&data.ptr);
		fflog(" %dms", r / msec_bytes);
		if (r < 0)
			fflog("ffaudio.read: %s", audio->error(b));
		x(r >= 0);
		data.len = r;

		ffstderr_write(data.ptr, data.len);

	}

	fflog("ffaudio.stop...");
	r = audio->stop(b);
	if (r != 0)
		fflog("ffaudio.stop: %s", audio->error(b));
	xieq(0, r);

	fflog("ffaudio.clear...");
	r = audio->clear(b);
	if (r != 0)
		fflog("ffaudio.clear: %s", audio->error(b));
	xieq(0, r);

	fflog("ffaudio.start...");
	r = audio->start(b);
	if (r != 0)
		fflog("ffaudio.start: %s", audio->error(b));
	xieq(0, r);

	audio->free(b);
	fflog("record done");

    audio->uninit();

    return NULL;
}

void list(ffaudio_interface *audio)
{
	ffaudio_dev *d;

	// FFAUDIO_DEV_PLAYBACK, FFAUDIO_DEV_CAPTURE
	static const char* const mode[] = { "playback", "capture" };
	for (ffuint i = 0;  i != 2;  i++) {
		fflog("%s devices:", mode[i]);
		d = audio->dev_alloc(i);
		x(d != NULL);

		for (;;) {
			int r = audio->dev_next(d);
			if (r > 0)
				break;
			else if (r < 0) {
				fflog("error: %s", audio->dev_error(d));
				break;
			}

			fflog("device: name: '%s'  id: '%s'  default: %s"
				, audio->dev_info(d, FFAUDIO_DEV_NAME)
				, audio->dev_info(d, FFAUDIO_DEV_ID)
				, audio->dev_info(d, FFAUDIO_DEV_IS_DEFAULT)
				);
		}

		audio->dev_free(d);
	}
}

int audioio_init(char *capture_dev, char *playback_dev, int audio_subsys)
{
    // TODO: initialize the buffers...

    audio_subsystem = audio_subsys;
    pthread_t radio_capture, radio_playback;

    pthread_create(&radio_capture, NULL, radio_capture_thread, (void*)capture_dev);
    pthread_create(&radio_playback, NULL, radio_playback_thread, (void*)playback_dev);

    pthread_join(radio_capture, NULL);
    pthread_join(radio_playback, NULL);


	return 0;
}

int main()
{
//    char *radio_capture_dev = "plughw:0,0";
//    char *radio_playback_dev = "plughw:0,0";

    char *radio_capture_dev = NULL;
    char *radio_playback_dev = NULL;
    return audioio_init(radio_capture_dev, radio_playback_dev, AUDIO_SUBSYSTEM_PULSE);

}
