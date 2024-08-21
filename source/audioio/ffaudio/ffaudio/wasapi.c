/** ffaudio: WASAPI wrapper
2020, Simon Zolin
*/

#include <ffaudio/audio.h>
#include <ffbase/string.h>

#define COBJMACROS
#include <mmdeviceapi.h>
#include <audioclient.h>


static char* wasapi_error(const char *errfunc, ffuint err);
static int wasapi_release(ffaudio_buf *b);
static int wasapi_format_mix(IAudioClient *client, ffaudio_conf *conf, const char **errfunc);


int ffwasapi_init(ffaudio_init_conf *conf)
{
	CoInitializeEx(NULL, 0);
	return 0;
}

void ffwasapi_uninit()
{
}


struct ffaudio_dev {
	IMMDeviceCollection *dcoll;
	ffuint mode;
	ffuint idx;
	wchar_t *def_id;
	ffuint def_format[3];

	const char *errfunc;
	char *errmsg;
	ffsize err;

	wchar_t *id;
	char *name;
	ffuint is_default :1;
};

static const GUID _CLSID_MMDeviceEnumerator = {0xbcde0395, 0xe52f, 0x467c, {0x8e,0x3d, 0xc4,0x57,0x92,0x91,0x69,0x2e}};
static const GUID _IID_IMMDeviceEnumerator = {0xa95664d2, 0x9614, 0x4f35, {0xa7,0x46, 0xde,0x8d,0xb6,0x36,0x17,0xe6}};
static const GUID _IID_IAudioRenderClient = {0xf294acfc, 0x3146, 0x4483, {0xa7,0xbf, 0xad,0xdc,0xa7,0xc2,0x60,0xe2}};
static const GUID _IID_IAudioCaptureClient = {0xc8adbd64, 0xe71e, 0x48a0, {0xa4,0xde, 0x18,0x5c,0x39,0x5c,0xd3,0x17}};
static const GUID _IID_IAudioClient = {0x1cb9ad4c, 0xdbfa, 0x4c32, {0xb1,0x78, 0xc2,0xf5,0x68,0xa7,0x03,0xb2}};
static const PROPERTYKEY _PKEY_Device_FriendlyName = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14}; // DEVPROP_TYPE_STRING
static const PROPERTYKEY _PKEY_AudioEngine_DeviceFormat = {{0xf19f064d, 0x082c, 0x4e27, {0xbc,0x73,0x68,0x82,0xa1,0xbb,0x8e,0x4c}}, 0};

ffaudio_dev* ffwasapi_dev_alloc(ffuint mode)
{
	ffaudio_dev *d = ffmem_new(ffaudio_dev);
	if (d == NULL)
		return NULL;
	d->mode = mode;
	return d;
}

void ffwasapi_dev_free(ffaudio_dev *d)
{
	if (d == NULL)
		return;
	if (d->def_id != NULL)
		CoTaskMemFree(d->def_id);
	if (d->id != NULL)
		CoTaskMemFree(d->id);
	if (d->dcoll != NULL)
		IMMDeviceCollection_Release(d->dcoll);
	ffmem_free(d->name);
	ffmem_free(d->errmsg);
	ffmem_free(d);
}

int ffwasapi_dev_next(ffaudio_dev *d)
{
	int rc = -FFAUDIO_ERROR;
	HRESULT r = 0;
	IMMDeviceEnumerator *enu = NULL;
	IMMDevice *dev = NULL;
	IPropertyStore *props = NULL;
	PROPVARIANT name;
	PropVariantInit(&name);
	d->errfunc = NULL;

	if (d->dcoll == NULL) {
		if (0 != (r = CoCreateInstance(&_CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, &_IID_IMMDeviceEnumerator, (void**)&enu))) {
			d->errfunc = "CoCreateInstance";
			goto end;
		}

		ffuint mode = (d->mode == FFAUDIO_DEV_PLAYBACK) ? eRender : eCapture;
		if (0 != (r = IMMDeviceEnumerator_EnumAudioEndpoints(enu, mode, DEVICE_STATE_ACTIVE, &d->dcoll))) {
			d->errfunc = "IMMDeviceEnumerator_EnumAudioEndpoints";
			goto end;
		}

		if (0 == IMMDeviceEnumerator_GetDefaultAudioEndpoint(enu, mode, eConsole, &dev)) {
			IMMDevice_GetId(dev, &d->def_id);
			IMMDevice_Release(dev);
		}
		dev = NULL;
	}

	if (0 != (r = IMMDeviceCollection_Item(d->dcoll, d->idx++, &dev))) {
		rc = 1;
		goto end;
	}

	if (0 != (r = IMMDevice_OpenPropertyStore(dev, STGM_READ, &props))) {
		d->errfunc = "IMMDevice_OpenPropertyStore";
		goto end;
	}

	if (0 != (r = IPropertyStore_GetValue(props, &_PKEY_Device_FriendlyName, &name))) {
		d->errfunc = "IPropertyStore_GetValue";
		goto end;
	}

	if (0 != (r = IMMDevice_GetId(dev, &d->id))) {
		d->errfunc = "IMMDevice_GetId";
		goto end;
	}

	if (d->def_id != NULL)
		d->is_default = !wcscmp(d->id, d->def_id);

	if (NULL == (d->name = ffsz_alloc_wtou(name.pwszVal))) {
		r = GetLastError();
		rc = -FFAUDIO_ERROR;
		goto end;
	}

	rc = 0;

end:
	PropVariantClear(&name);
	if (dev != NULL)
		IMMDevice_Release(dev);
	if (props != NULL)
		IPropertyStore_Release(props);
	if (enu != NULL)
		IMMDeviceEnumerator_Release(enu);
	d->err = r;
	return rc;
}

static int wasapi_dev_format_mix(ffaudio_dev *d)
{
	int r;
	IMMDeviceEnumerator *enu = NULL;
	IMMDevice *mmdev = NULL;
	IAudioClient *cli = NULL;
	if (0 != (r = CoCreateInstance(&_CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, &_IID_IMMDeviceEnumerator, (void**)&enu)))
		goto fail;
	if (0 != (r = IMMDeviceEnumerator_GetDevice(enu, d->id, &mmdev)))
		goto fail;
	if (0 != (r = IMMDevice_Activate(mmdev, &_IID_IAudioClient, CLSCTX_ALL, NULL, (void**)&cli)))
		goto fail;

	ffaudio_conf conf = {};
	if (0 != (r = wasapi_format_mix(cli, &conf, &d->errfunc)))
		goto fail;
	d->def_format[0] = conf.format;
	d->def_format[1] = conf.sample_rate;
	d->def_format[2] = conf.channels;
	r = 0;

fail:
	if (cli != NULL)
		IAudioClient_Release(cli);
	if (mmdev != NULL)
		IMMDevice_Release(mmdev);
	if (enu != NULL)
		IMMDeviceEnumerator_Release(enu);
	return r;
}

const char* ffwasapi_dev_info(ffaudio_dev *d, ffuint i)
{
	switch (i) {
	case FFAUDIO_DEV_ID:
		return (char*)d->id;

	case FFAUDIO_DEV_NAME:
		return d->name;

	case FFAUDIO_DEV_IS_DEFAULT:
		return (d->is_default) ? "1" : NULL;

	case FFAUDIO_DEV_MIX_FORMAT:
		if (0 != wasapi_dev_format_mix(d))
			return NULL;
		return (char*)d->def_format;
	}
	return NULL;
}

const char* ffwasapi_dev_error(ffaudio_dev *d)
{
	ffmem_free(d->errmsg);
	d->errmsg = wasapi_error(d->errfunc, d->err);
	return d->errmsg;
}


struct ffaudio_buf {
	IAudioClient *client;
	IAudioRenderClient *render;
	IAudioCaptureClient *capt;
	HANDLE event;

	// exclusive mode playback
	ffbyte *buf; // store incomplete chunks of user data
	ffuint buf_off;

	int filled_buffers;
	ffuint frame_size;
	ffuint n_frames;
	ffuint buf_frames;
	ffuint max_ms, period_ms;
	ffuint started;
	ffuint nonblock;
	ffuint notify_unsync;
	ffuint user_driven;

	const char *errfunc;
	char *errmsg;
	int err;
};

ffaudio_buf* ffwasapi_alloc()
{
	ffaudio_buf *b = ffmem_new(ffaudio_buf);
	if (b == NULL)
		return NULL;
	return b;
}

static void wasapi_close(ffaudio_buf *b)
{
	if (b->render != NULL) {
		IAudioRenderClient_Release(b->render);
		b->render = NULL;
	}
	if (b->capt != NULL) {
		IAudioCaptureClient_Release(b->capt);
		b->capt = NULL;
	}
	if (b->client != NULL) {
		IAudioClient_Release(b->client);
		b->client = NULL;
	}
	if (b->event != NULL) {
		CloseHandle(b->event);
		b->event = NULL;
	}
	ffmem_free(b->buf);
	b->buf = NULL;
}

void ffwasapi_free(ffaudio_buf *b)
{
	if (b == NULL)
		return;

	wasapi_close(b);
	ffmem_free(b->errmsg);
	ffmem_free(b);
}

const char* ffwasapi_error(ffaudio_buf *b)
{
	return wasapi_error(b->errfunc, b->err);
}

static const GUID wfx_guid = { 1, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71} };

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

/** ffaudio format -> WAVEFORMATEXTENSIBLE */
static void wfx_from_ff(WAVEFORMATEXTENSIBLE *wf, const ffaudio_conf *conf)
{
	wf_from_ff(&wf->Format, conf);
	wf->Format.wFormatTag = 0xfffe;
	wf->Format.cbSize = 22;

	wf->Samples.wValidBitsPerSample = conf->format & 0xff;
	if (conf->format == FFAUDIO_F_INT24_4)
		wf->Samples.wValidBitsPerSample = 24;

	ffmem_copy(&wf->SubFormat, &wfx_guid, sizeof(wfx_guid));
	if (conf->format == FFAUDIO_F_FLOAT32)
		*(ffushort*)&wf->SubFormat = 3;

	wf->dwChannelMask = 3;
}

/** WAVEFORMATEXTENSIBLE format -> ffaudio format */
static int wfx_to_ff_format(const WAVEFORMATEXTENSIBLE *wf)
{
	ffuint f, fmt, bps, bps_store;

	f = wf->Format.wFormatTag;
	bps = bps_store = wf->Format.wBitsPerSample;
	if (wf->Format.wFormatTag == 0xfffe) {
		if (wf->Format.cbSize != 22)
			return -1;
		f = *(ffushort*)&wf->SubFormat;
		bps = wf->Samples.wValidBitsPerSample;
	}

	switch (f) {
	case 1:
		switch (bps) {
		case 8:
		case 16:
		case 24:
		case 32:
			if (bps == bps_store)
				fmt = bps;
			else if (bps == 24 && bps_store == 32)
				fmt = FFAUDIO_F_INT24_4;
			else
				return -1;
			break;

		default:
			return -1;
		}
		break;

	case 3:
		fmt = FFAUDIO_F_FLOAT32;
		break;

	default:
		return -1;
	}

	return fmt;
}

/** Get shared mode mix format */
static int wasapi_format_mix(IAudioClient *client, ffaudio_conf *conf, const char **errfunc)
{
	HRESULT r;
	WAVEFORMATEX *wf = NULL;

	if (0 != (r = IAudioClient_GetMixFormat(client, &wf))) {
		*errfunc = "IAudioClient_GetMixFormat";
		goto end;
	}

	conf->format = FFAUDIO_F_FLOAT32;
	conf->sample_rate = wf->nSamplesPerSec;
	conf->channels = wf->nChannels;

end:
	if (wf != NULL)
		CoTaskMemFree(wf);
	return r;
}

/** Get format which is specified in OS as default. */
static int wasapi_format_default(IMMDevice *dev, ffaudio_conf *conf, const char **errfunc)
{
	HRESULT r;
	IPropertyStore *store = NULL;
	PROPVARIANT prop;
	PropVariantInit(&prop);

	if (0 != (r = IMMDevice_OpenPropertyStore(dev, STGM_READ, &store))) {
		*errfunc = "IMMDevice_OpenPropertyStore";
		goto end;
	}

	if (0 != (r = IPropertyStore_GetValue(store, &_PKEY_AudioEngine_DeviceFormat, &prop))) {
		*errfunc = "IPropertyStore_GetValue";
		goto end;
	}

	const WAVEFORMATEXTENSIBLE *wf = (void*)prop.blob.pBlobData;
	int rr = wfx_to_ff_format(wf);
	if (rr < 0) {
		*errfunc = "";
		r = AUDCLNT_E_UNSUPPORTED_FORMAT;
		goto end;
	}

	conf->format = rr;
	conf->sample_rate = wf->Format.nSamplesPerSec;
	conf->channels = wf->Format.nChannels;
	r = 0;

end:
	PropVariantClear(&prop);
	if (store != NULL)
		IPropertyStore_Release(store);
	return r;
}

/** Check if the format is supported
For shared mode 'closed-match' pointer must be deallocated */
static int wasapi_fmt_supported(IAudioClient *client, ffuint mode, const ffaudio_conf *conf, WAVEFORMATEXTENSIBLE *wf, WAVEFORMATEX **owf)
{
	wfx_from_ff(wf, conf);
	HRESULT r = IAudioClient_IsFormatSupported(client, mode, (void*)wf, owf);
	if (r == S_FALSE)
		CoTaskMemFree(*owf);
	return r;
}

/** Find the supported format (with the same sample rate):
. Try input format
. Try other known formats
. Try known formats with different channels number
Return 0 if input format is supported;
 >0 if new format is set;
 <0 if none of the known formats is supported */
static int wasapi_find_fmt(IAudioClient *client, IMMDevice *dev, ffaudio_conf *conf, WAVEFORMATEXTENSIBLE *wf, ffuint excl)
{
	ffaudio_conf f;
	WAVEFORMATEXTENSIBLE *owfx = NULL;
	WAVEFORMATEX **owf = (excl) ? NULL : (void*)&owfx;
	ffuint mode = (excl) ? AUDCLNT_SHAREMODE_EXCLUSIVE : AUDCLNT_SHAREMODE_SHARED;

	if (0 == wasapi_fmt_supported(client, mode, conf, wf, owf))
		return 0;

	static const ffushort fmts[] = {
		FFAUDIO_F_FLOAT32,
		FFAUDIO_F_INT32,
		FFAUDIO_F_INT24,
		FFAUDIO_F_INT24_4,
		FFAUDIO_F_INT16,
		FFAUDIO_F_UINT8,
	};

	f.format = conf->format;
	f.sample_rate = conf->sample_rate;
	f.channels = conf->channels;
	for (ffuint i = 0;  i != FF_COUNT(fmts);  i++) {
		f.format = fmts[i];
		if (0 == wasapi_fmt_supported(client, mode, &f, wf, owf)) {
			conf->format = fmts[i];
			return 1;
		}
	}

	f.channels = (conf->channels == 2) ? 1 : 2;
	for (ffuint i = 0;  i != FF_COUNT(fmts);  i++) {
		f.format = fmts[i];
		if (0 == wasapi_fmt_supported(client, mode, &f, wf, owf)) {
			conf->format = fmts[i];
			conf->channels = f.channels;
			return 1;
		}
	}

	return -1;
}

/** bytes -> msec:
size*1000/(rate*width*channels) */
static ffuint buffer_size_to_msec(const ffaudio_conf *conf, ffuint size)
{
	return size * 1000 / (conf->sample_rate * (conf->format & 0xff) / 8 * conf->channels);
}

/* Algorithm for opening WASAPI buffer:

. Get device enumerator
. Get device (default or specified by ID)
. Get general audio client object
. Find a (probably) supported format
  * input format is supported:
  * new format is set:
    . Go on
  * no format is supported:
    . Get mix format (shared mode)
    . Get OS default format (exclusive mode)
. Open device via general audio object
  * Success (but new format is set):
    . Fail with FFAUDIO_EFORMAT
  * AUDCLNT_E_UNSUPPORTED_FORMAT (shared mode):
    . Get mix format
    . Try again (once)
  * AUDCLNT_E_UNSUPPORTED_FORMAT (exclusive mode):
    . Get OS default format
    . Try again (once)
  * E_POINTER (shared mode):
    . Convert WF-extensible -> WFEX
    . Try again (once)
  * AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED (exclusive mode):
    . Set aligned buffer size
    . Try again (once)
. Get specific (render/capture) audio object
*/
int ffwasapi_open(ffaudio_buf *b, ffaudio_conf *conf, ffuint flags)
{
	int rc = FFAUDIO_ERROR;
	HRESULT r;
	IMMDeviceEnumerator *enu = NULL;
	IMMDevice *dev = NULL;
	WAVEFORMATEXTENSIBLE wf;
	int new_format = 0; // 0:use input format; 1:new format is set; -1:default format is set
	ffuint find_format = 1;
	ffuint buf_align = 0;
	ffuint e_pointer = 0;
	ffuint excl = !!(flags & FFAUDIO_O_EXCLUSIVE);
	ffuint loopback = ((flags & 0x0f) == FFAUDIO_LOOPBACK);
	ffuint capture = ((flags & 0x0f) == FFAUDIO_CAPTURE) || loopback;
	ffuint events = ((flags & (FFAUDIO_O_EXCLUSIVE | FFAUDIO_LOOPBACK)) == FFAUDIO_O_EXCLUSIVE);
	b->nonblock = !!(flags & FFAUDIO_O_NONBLOCK);
	b->notify_unsync = !!(flags & FFAUDIO_O_UNSYNC_NOTIFY);

	if (conf->buffer_length_msec == 0)
		conf->buffer_length_msec = 500;
	REFERENCE_TIME dur = conf->buffer_length_msec * 1000 * 10;
	if (excl)
		dur /= 2;

	if (0 != (r = CoCreateInstance(&_CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, &_IID_IMMDeviceEnumerator, (void**)&enu))) {
		b->errfunc = "CoCreateInstance";
		goto end;
	}

	if (conf->device_id == NULL) {
		ffuint mode = (capture && !loopback) ? eCapture : eRender;
		if (0 != (r = IMMDeviceEnumerator_GetDefaultAudioEndpoint(enu, mode, eConsole, &dev))) {
			b->errfunc = "IMMDeviceEnumerator_GetDefaultAudioEndpoint";
			goto end;
		}
	} else {
		if (0 != (r = IMMDeviceEnumerator_GetDevice(enu, (wchar_t*)conf->device_id, &dev))) {
			b->errfunc = "IMMDeviceEnumerator_GetDevice";
			goto end;
		}
	}

	wfx_from_ff(&wf, conf);

	for (;;) {

		if (0 != (r = IMMDevice_Activate(dev, &_IID_IAudioClient, CLSCTX_ALL, NULL, (void**)&b->client))) {
			b->errfunc = "IMMDevice_Activate";
			goto end;
		}

		if (find_format) {
			find_format = 0;
			new_format = wasapi_find_fmt(b->client, dev, conf, &wf, excl);
			if (new_format < 0) {
				if (!excl) {
					if (0 != (r = wasapi_format_mix(b->client, conf, &b->errfunc)))
						goto end;
				} else {
					if (0 != (r = wasapi_format_default(dev, conf, &b->errfunc)))
						goto end;
				}
				wfx_from_ff(&wf, conf);
			}
		}

		ffuint mode = (excl) ? AUDCLNT_SHAREMODE_EXCLUSIVE : AUDCLNT_SHAREMODE_SHARED;
		ffuint aflags = (loopback) ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0;
		aflags |= (events) ? AUDCLNT_STREAMFLAGS_EVENTCALLBACK : 0;
		r = IAudioClient_Initialize(b->client, mode, aflags, dur, dur, (void*)&wf, NULL);
		if (r == 0) {
			if (new_format != 0) {
				rc = FFAUDIO_EFORMAT;
				goto end;
			}
			break;
		}

		b->errfunc = "IAudioClient_Initialize";
		switch (r) {

		case E_POINTER:
			if (e_pointer || excl)
				goto end;
			e_pointer = 1;

			if (conf->format == FFAUDIO_F_INT24_4) {
				new_format = -1;
				if (0 != (r = wasapi_format_mix(b->client, conf, &b->errfunc)))
					goto end;
			}

			wf_from_ff(&wf.Format, conf);
			break;

		case AUDCLNT_E_UNSUPPORTED_FORMAT:
			if (new_format < 0)
				goto end; // even the default format isn't supported

			// the format approved by IAudioClient_IsFormatSupported() isn't actually supported
			new_format = -1;
			if (!excl) {
				if (0 != (r = wasapi_format_mix(b->client, conf, &b->errfunc)))
					goto end;
			} else {
				if (0 != (r = wasapi_format_default(dev, conf, &b->errfunc)))
					goto end;
			}
			wfx_from_ff(&wf, conf);
			break;

		case AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED: {
			if (buf_align)
				goto end;
			buf_align = 1;

			ffuint buf_frames;
			if (0 != (r = IAudioClient_GetBufferSize(b->client, &buf_frames))) {
				b->errfunc = "IAudioClient_GetBufferSize";
				b->err = r;
				goto end;
			}

			// Get an aligned buffer size.  The formula is from MSDN
			dur = (REFERENCE_TIME)((10000.0 * 1000 / conf->sample_rate * buf_frames) + 0.5);
			break;
		}

		default:
			goto end;
		}

		IAudioClient_Release(b->client);
		b->client = NULL;
	}

	if (events) {
		if (NULL == (b->event = CreateEvent(NULL, 0, 0, NULL))) {
			b->errfunc = "CreateEvent";
			b->err = GetLastError();
			rc = FFAUDIO_ERROR;
			goto end;
		}
		if (0 != (r = IAudioClient_SetEventHandle(b->client, b->event))) {
			b->errfunc = "IAudioClient_SetEventHandle";
			goto end;
		}
	}

	if (0 != (r = IAudioClient_GetBufferSize(b->client, &b->buf_frames))) {
		b->errfunc = "IAudioClient_GetBufferSize";
		b->err = r;
		goto end;
	}

	const void *type = (capture) ? &_IID_IAudioCaptureClient : &_IID_IAudioRenderClient;
	void **target = (capture) ? (void**)&b->capt : (void**)&b->render;
	if (0 != (r = IAudioClient_GetService(b->client, type, target))) {
		b->errfunc = "IAudioClient_GetService";
		goto end;
	}

	b->frame_size = (conf->format & 0xff) / 8 * conf->channels;
	conf->buffer_length_msec = buffer_size_to_msec(conf, b->buf_frames * b->frame_size);
	b->period_ms = conf->buffer_length_msec / 4;
	if (events) {
		if (NULL == (b->buf = ffmem_alloc(b->buf_frames * b->frame_size))) {
			b->err = ERROR_NOT_ENOUGH_MEMORY;
			goto end;
		}
		conf->buffer_length_msec *= 2;
	}
	b->max_ms = conf->buffer_length_msec;
	if (events && (flags & FFAUDIO_O_USER_EVENTS)) {
		b->user_driven = 1;
		conf->event_h = b->event;
	}
	rc = 0;

end:
	if (rc != 0) {
		wasapi_close(b);
		if (rc == FFAUDIO_ERROR)
			b->err = r;
	}
	if (dev != NULL)
		IMMDevice_Release(dev);
	if (enu != NULL)
		IMMDeviceEnumerator_Release(enu);
	return rc;
}

int ffwasapi_start(ffaudio_buf *b)
{
	if (b->started)
		return 0;

	int r;
	if (0 != (r = IAudioClient_Start(b->client))) {
		b->errfunc = "IAudioClient_Start";
		b->err = r;
		return FFAUDIO_ERROR;
	}

	b->started = 1;
	return 0;
}

int ffwasapi_stop(ffaudio_buf *b)
{
	if (!b->started)
		return 0;

	int r;
	if (0 != (r = IAudioClient_Stop(b->client))) {
		b->errfunc = "IAudioClient_Stop";
		b->err = r;
		return FFAUDIO_ERROR;
	}

	b->started = 0;
	return 0;
}

int ffwasapi_clear(ffaudio_buf *b)
{
	int r;

	wasapi_release(b);

	if (0 != (r = IAudioClient_Reset(b->client))) {
		b->errfunc = "IAudioClient_Reset";
		b->err = r;
		return FFAUDIO_ERROR;
	}
	return 0;
}

/** Write 1 data chunk */
static int wasapi_writeonce(ffaudio_buf *b, const void *data, ffsize len)
{
	int r;
	ffbyte *d;

	ffuint filled;
	if (0 != (r = IAudioClient_GetCurrentPadding(b->client, &filled))) {
		b->errfunc = "IAudioClient_GetCurrentPadding";
		b->err = r;
		return -FFAUDIO_ERROR;
	}

	ffuint n = b->buf_frames - filled;
	if (n == 0)
		return 0;

	n = ffmin(len / b->frame_size, n);
	if (0 != (r = IAudioRenderClient_GetBuffer(b->render, n, &d))) {
		b->errfunc = "IAudioRenderClient_GetBuffer";
		b->err = r;
		return -FFAUDIO_ERROR;
	}

	ffmem_copy(d, data, n * b->frame_size);

	if (0 != (r = IAudioRenderClient_ReleaseBuffer(b->render, n, 0))) {
		b->errfunc = "IAudioRenderClient_ReleaseBuffer";
		b->err = r;
		return -FFAUDIO_ERROR;
	}

	return n * b->frame_size;
}

/** Read 1 data chunk */
static int wasapi_readonce(ffaudio_buf *b, const void **data)
{
	int r;
	ffbyte *d;

	if (b->n_frames != 0) {
		r = IAudioCaptureClient_ReleaseBuffer(b->capt, b->n_frames);
		b->n_frames = 0;
		if (r != 0) {
			b->errfunc = "IAudioCaptureClient_ReleaseBuffer";
			b->err = r;
			return -FFAUDIO_ERROR;
		}
	}

	DWORD flags;
	if (0 != (r = IAudioCaptureClient_GetBuffer(b->capt, &d, &b->n_frames, &flags, NULL, NULL))) {
		if (r == AUDCLNT_S_BUFFER_EMPTY) {
			return 0;
		}
		b->errfunc = "IAudioCaptureClient_GetBuffer";
		b->err = r;
		return -FFAUDIO_ERROR;
	}

	*data = d;
	return b->n_frames * b->frame_size;
}

/** Release the current data chunk */
static int wasapi_release(ffaudio_buf *b)
{
	int r;
	if (b->n_frames != 0) {
		r = IAudioCaptureClient_ReleaseBuffer(b->capt, b->n_frames);
		b->n_frames = 0;
		if (r != 0) {
			b->errfunc = "IAudioCaptureClient_ReleaseBuffer";
			b->err = r;
			return -FFAUDIO_ERROR;
		}
	}

	b->filled_buffers = 0;
	return 0;
}

/** Write 1 data chunk to a stream in exclusive mode */
static int wasapi_writeonce_excl(ffaudio_buf *b, const void *data, ffsize len)
{
	int r;

	if (b->filled_buffers == 2) {
		return 0;

	} else if (b->filled_buffers < 0) {
		// underrun
		b->filled_buffers = 0;
		ffwasapi_stop(b);
		return -FFAUDIO_ESYNC;
	}

	const void *d = data;
	ffuint bufsize = b->buf_frames * b->frame_size;
	ffuint n = bufsize;
	if (b->buf_off != 0 || len < bufsize) {
		// cache user data until we have a full buffer
		n = ffmin(len, bufsize - b->buf_off);
		ffmem_copy(&b->buf[b->buf_off], data, n);
		b->buf_off += n;
		if (b->buf_off != bufsize)
			return n;
		b->buf_off = 0;
		d = b->buf;
	}

	ffbyte *hwbuf;
	if (0 != (r = IAudioRenderClient_GetBuffer(b->render, b->buf_frames, &hwbuf))) {
		b->errfunc = "IAudioRenderClient_GetBuffer";
		b->err = r;
		return -FFAUDIO_ERROR;
	}

	ffmem_copy(hwbuf, d, bufsize);

	if (0 != (r = IAudioRenderClient_ReleaseBuffer(b->render, b->buf_frames, 0))) {
		b->errfunc = "IAudioRenderClient_ReleaseBuffer";
		b->err = r;
		return -FFAUDIO_ERROR;
	}
	b->filled_buffers++;
	return n;
}

static int wasapi_return_error(ffaudio_buf *b)
{
	if (b->err == AUDCLNT_E_DEVICE_INVALIDATED)
		return -FFAUDIO_EDEV_OFFLINE;
	return -FFAUDIO_ERROR;
}

/** Check for device error.
Seems that IAudioClient_GetBufferSize() has the minimum latency.
Return enum FFAUDIO_E (negative) */
static int wasapi_dev_status(ffaudio_buf *b)
{
	int r;
	ffuint n;
	if (0 != (r = IAudioClient_GetBufferSize(b->client, &n))) {
		b->errfunc = "IAudioClient_GetBufferSize";
		b->err = r;
		return wasapi_return_error(b);
	}
	return 0;
}

/*
In exclusive mode we have 2 buffers (or 2 halves of the buffer) which we fill one by one.
When both buffers are filled, we start the stream if it's not started.
Then we wait for the event to signal which means that 1 buffer has been played.
On underrun the sound continues to play in a loop, so the manual stop and reset are required.
*/
static int wasapi_write_excl(ffaudio_buf *b, const void *data, ffsize len)
{
	for (;;) {
		int r = wasapi_writeonce_excl(b, data, len);
		if (r > 0) {
			return r;
		} else if (r < 0) {
			if (r == -FFAUDIO_ESYNC && !b->notify_unsync)
				continue;
			else if (r == -FFAUDIO_ERROR)
				return wasapi_return_error(b);
			return r;
		}

		if (0 != (r = ffwasapi_start(b)))
			return -r;

		if (b->user_driven)
			return 0;

		ffuint t = (b->nonblock) ? 0 : b->max_ms;
		r = WaitForSingleObject(b->event, t);
		if (r != WAIT_OBJECT_0) {
			if (r == WAIT_TIMEOUT) {
				if (0 != (r = wasapi_dev_status(b)))
					return r;
				if (b->nonblock)
					return 0;
				SetLastError(ERROR_SEM_TIMEOUT);
			}
			b->errfunc = "WaitForSingleObject";
			b->err = GetLastError();
			return -FFAUDIO_ERROR;
		}
		b->filled_buffers--;
	}
}

/*
In shared mode with AUDCLNT_STREAMFLAGS_EVENTCALLBACK too many events from WASAPI may be triggerred, regardless of buffer size.
This behaviour generates many unnecessary context switches while all we need is to be notified just 2 times per buffer.
Furthermore, AUDCLNT_STREAMFLAGS_EVENTCALLBACK doesn't work together with AUDCLNT_STREAMFLAGS_LOOPBACK.
*/
int ffwasapi_write(ffaudio_buf *b, const void *data, ffsize len)
{
	if (b->event != NULL)
		return wasapi_write_excl(b, data, len);

	for (;;) {
		int r = wasapi_writeonce(b, data, len);
		if (r > 0)
			return r;
		else if (r < 0) {
			if (r == -FFAUDIO_ERROR)
				return wasapi_return_error(b);
			return r;
		}

		if (0 != (r = ffwasapi_start(b)))
			return -r;

		if (b->nonblock)
			return 0;

		Sleep(b->period_ms);
	}
}

static int wasapi_drain_excl(ffaudio_buf *b)
{
	int r;
	for (;;) {
		if (b->filled_buffers == 0)
			return 1;

		if (b->user_driven)
			return 0;

		r = WaitForSingleObject(b->event, b->max_ms);
		if (r != WAIT_OBJECT_0) {
			if (r == WAIT_TIMEOUT) {
				if (0 != (r = wasapi_dev_status(b)))
					return r;
				SetLastError(ERROR_SEM_TIMEOUT);
			}
			b->errfunc = "WaitForSingleObject";
			b->err = GetLastError();
			return -FFAUDIO_ERROR;
		}
		b->filled_buffers--;
	}
}

int ffwasapi_drain(ffaudio_buf *b)
{
	if (b->event != NULL)
		return wasapi_drain_excl(b);

	int r;
	ffuint filled;
	for (;;) {
		if (0 != (r = IAudioClient_GetCurrentPadding(b->client, &filled))) {
			b->errfunc = "IAudioClient_GetCurrentPadding";
			b->err = r;
			return wasapi_return_error(b);
		}
		if (filled == 0)
			return 1;

		if (0 != (r = ffwasapi_start(b)))
			return -r;

		if (b->nonblock)
			return 0;

		Sleep(b->period_ms);
	}
}

static int wasapi_read_excl(ffaudio_buf *b, const void **data)
{
	int r;
	for (;;) {
		if (b->filled_buffers != 0
			&& 0 != (r = wasapi_readonce(b, data))) {
			if (r > 0)
				b->filled_buffers--;
			else if (r == -FFAUDIO_ERROR)
				return wasapi_return_error(b);
			return r;
		}

		if (0 != (r = ffwasapi_start(b)))
			return -r;

		if (b->user_driven)
			return 0;

		ffuint t = (b->nonblock) ? 0 : b->max_ms;
		r = WaitForSingleObject(b->event, t);
		if (r != WAIT_OBJECT_0) {
			if (r == WAIT_TIMEOUT) {
				if (0 != (r = wasapi_dev_status(b)))
					return r;
				if (b->nonblock)
					return 0;
				SetLastError(ERROR_SEM_TIMEOUT);
			}
			b->errfunc = "WaitForSingleObject";
			b->err = GetLastError();
			return -FFAUDIO_ERROR;
		}
		b->filled_buffers++;
	}
}

int ffwasapi_read(ffaudio_buf *b, const void **data)
{
	if (b->event != NULL)
		return wasapi_read_excl(b, data);

	for (;;) {
		int r = wasapi_readonce(b, data);
		if (r > 0) {
			return r;
		} else if (r < 0) {
			if (r == -FFAUDIO_ERROR)
				return wasapi_return_error(b);
			return r;
		}

		if (0 != (r = ffwasapi_start(b)))
			return -r;

		if (b->nonblock)
			return 0;

		Sleep(b->period_ms);
	}
}

void ffwasapi_signal(ffaudio_buf *b)
{
	if (!b->user_driven) return;

	if (b->capt)
		b->filled_buffers++;
	else
		b->filled_buffers--;
}


static const char audclnt_errstr[][39] = {
	"",
	"AUDCLNT_E_NOT_INITIALIZED", // 0x1
	"AUDCLNT_E_ALREADY_INITIALIZED", // 0x2
	"AUDCLNT_E_WRONG_ENDPOINT_TYPE", // 0x3
	"AUDCLNT_E_DEVICE_INVALIDATED", // 0x4
	"AUDCLNT_E_NOT_STOPPED", // 0x5
	"AUDCLNT_E_BUFFER_TOO_LARGE", // 0x6
	"AUDCLNT_E_OUT_OF_ORDER", // 0x7
	"AUDCLNT_E_UNSUPPORTED_FORMAT", // 0x8
	"AUDCLNT_E_INVALID_SIZE", // 0x9
	"AUDCLNT_E_DEVICE_IN_USE", // 0xa
	"AUDCLNT_E_BUFFER_OPERATION_PENDING", // 0xb
	"AUDCLNT_E_THREAD_NOT_REGISTERED", // 0xc
	"",
	"AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED", // 0xe
	"AUDCLNT_E_ENDPOINT_CREATE_FAILED", // 0xf
	"AUDCLNT_E_SERVICE_NOT_RUNNING", // 0x10
	"AUDCLNT_E_EVENTHANDLE_NOT_EXPECTED", // 0x11
	"AUDCLNT_E_EXCLUSIVE_MODE_ONLY", // 0x12
	"AUDCLNT_E_BUFDURATION_PERIOD_NOT_EQUAL", // 0x13
	"AUDCLNT_E_EVENTHANDLE_NOT_SET", // 0x14
	"AUDCLNT_E_INCORRECT_BUFFER_SIZE", // 0x15
	"AUDCLNT_E_BUFFER_SIZE_ERROR", // 0x16
	"AUDCLNT_E_CPUUSAGE_EXCEEDED", // 0x17
	"AUDCLNT_E_BUFFER_ERROR", // 0x18
	"AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED", // 0x19
	"",
	"",
	"",
	"",
	"",
	"",
	"AUDCLNT_E_INVALID_DEVICE_PERIOD", // 0x20
	"AUDCLNT_E_INVALID_STREAM_FLAG", //  0x21
	"AUDCLNT_E_ENDPOINT_OFFLOAD_NOT_CAPABLE", //  0x22
	"AUDCLNT_E_OUT_OF_OFFLOAD_RESOURCES", //  0x23
	"AUDCLNT_E_OFFLOAD_MODE_ONLY", //  0x24
	"AUDCLNT_E_NONOFFLOAD_MODE_ONLY", //  0x25
	"AUDCLNT_E_RESOURCES_INVALIDATED", //  0x26
};

/** Return error string or NULL if it's not AUDCLNT code.  */
static const char* audclnt_error(ffuint err)
{
	if ((err & 0xffff0000) != (ffuint)MAKE_HRESULT(SEVERITY_ERROR, FACILITY_AUDCLNT, 0))
		return NULL;

	err = err & 0xffff;
	if (err >= FF_COUNT(audclnt_errstr))
		return "";

	return audclnt_errstr[err];
}

static char* wasapi_error(const char *errfunc, ffuint err)
{
	const char *estr = audclnt_error(err);
	if (estr != NULL) {
		char *s = ffsz_allocfmt("%s: (0x%xu) %s"
			, errfunc, err, estr);
		return s;
	}

	wchar_t buf[255];
	int n = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS | FORMAT_MESSAGE_MAX_WIDTH_MASK
		, 0, err, 0, buf, FF_COUNT(buf), 0);
	if (n == 0)
		buf[0] = '\0';

	char *s = ffsz_allocfmt("%s: %d (0x%xu) %q"
		, errfunc, err, err, buf);
	return s;
}


const struct ffaudio_interface ffwasapi = {
	ffwasapi_init,
	ffwasapi_uninit,

	ffwasapi_dev_alloc,
	ffwasapi_dev_free,
	ffwasapi_dev_error,
	ffwasapi_dev_next,
	ffwasapi_dev_info,

	ffwasapi_alloc,
	ffwasapi_free,
	ffwasapi_error,
	ffwasapi_open,
	ffwasapi_start,
	ffwasapi_stop,
	ffwasapi_clear,
	ffwasapi_write,
	ffwasapi_drain,
	ffwasapi_read,
	ffwasapi_signal,
};
