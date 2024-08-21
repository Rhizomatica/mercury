/** ffaudio: tester
2020, Simon Zolin
*/

#include <ffaudio/audio.h>
#include <ffbase/args.h>
#include <ffbase/stringz.h>
#include "std.h"
#include "audioio.h"
#ifdef FF_LINUX
#include <time.h>
#endif
typedef unsigned char u_char;

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


const ffaudio_interface *audio;
int skip_wav_header;

void list()
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

void record(ffaudio_conf *conf, ffuint flags)
{
	int r;
	ffaudio_buf *b;
	b = audio->alloc();
	x(b != NULL);

	ffstdout_fmt("ffaudio.open...");
	r = audio->open(b, conf, flags);
	if (r == FFAUDIO_EFORMAT) {
		ffstdout_fmt(" reopening...");
		r = audio->open(b, conf, flags);
	}
	if (r != 0)
		fflog("ffaudio.open: %d: %s", r, audio->error(b));
	xieq(0, r);
	fflog(" %d/%d/%d %dms"
		, conf->format, conf->sample_rate, conf->channels
		, conf->buffer_length_msec);

	ffuint msec_bytes = conf->sample_rate * conf->channels * (conf->format & 0xff) / 8 / 1000;
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
}

void play(ffaudio_conf *conf, ffuint flags)
{
	int r;
	ffaudio_buf *b;

	b = audio->alloc();
	x(b != NULL);

	ffstdout_fmt("ffaudio.open...");
	r = audio->open(b, conf, flags);
	if (r == FFAUDIO_EFORMAT)
		r = audio->open(b, conf, flags);
	if (r != 0)
		fflog("ffaudio.open: %d: %s", r, audio->error(b));
	xieq(0, r);
	fflog(" %d/%d/%d %dms"
		, conf->format, conf->sample_rate, conf->channels
		, conf->buffer_length_msec);

	ffuint frame_size = conf->channels * (conf->format & 0xff) / 8;
	ffuint sec_bytes = conf->sample_rate * conf->channels * (conf->format & 0xff) / 8;
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

		if (skip_wav_header) {
			static int wav_hdr_skip = 1;
			if (wav_hdr_skip) {
				wav_hdr_skip = 0;
				ffstr_shift(&data, 44);
			}
		}

		while (data.len >= frame_size) {
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
}

struct conf {
	const char *cmd;
	ffaudio_conf buf;
	ffuint flags;
	u_char exclusive;
	u_char hwdev;
	u_char loopback;
	u_char nonblock;
	u_char wav;
};

int conf_format(struct conf *c, ffstr s)
{
	if (ffstr_eqz(&s, "float32"))
		c->buf.format = FFAUDIO_F_FLOAT32;
	else if (ffstr_eqz(&s, "int32"))
		c->buf.format = FFAUDIO_F_INT32;
	else if (ffstr_eqz(&s, "int16"))
		c->buf.format = FFAUDIO_F_INT16;
	else if (ffstr_eqz(&s, "int8"))
		c->buf.format = FFAUDIO_F_INT8;
	else
		return 1;
	return 0;
}

int audioio_init()
{
	struct conf conf = {};
	conf.buf.app_name = "mercury_io";
	conf.buf.buffer_length_msec = 40;
	conf.buf.format = FFAUDIO_F_INT16;
	conf.buf.sample_rate = 48000;
	conf.buf.channels = 2;

	audio = ffaudio_default_interface();

    conf.flags = FFAUDIO_CAPTURE;
    // c.flags = FFAUDIO_PLAYBACK;

	//if (c.loopback) {
	//	c.flags &= ~0x0f;
	//	c.flags |= FFAUDIO_LOOPBACK;
	//}

    // conf.flags |= FFAUDIO_O_EXCLUSIVE;
	// conf.flags |= FFAUDIO_O_HWDEV;
	// conf.flags |= FFAUDIO_O_NONBLOCK;

	ffaudio_init_conf aconf = {};
	aconf.app_name = "mercury_io";
	xieq(0, audio->init(&aconf));

//	   list soundcards
//	list();

//	   capture
    record(&conf.buf, conf.flags);

//	   playback
//  play(&conf.buf, conf.flags);


	audio->uninit();
	return 0;
}

int main()
{

    return audioio_init();

}
