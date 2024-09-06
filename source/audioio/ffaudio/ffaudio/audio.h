/** ffaudio: user interface
2020, Simon Zolin
*/

#pragma once

#include <ffbase/base.h>


/** Error code */
enum FFAUDIO_E {
	FFAUDIO_ESUCCESS,
	FFAUDIO_ERROR,
	FFAUDIO_EFORMAT,
	FFAUDIO_ESYNC,
	FFAUDIO_EDEV_OFFLINE,

	/** PulseAudio: connection with server failed.
	Call uninit(), and then init() to reconnect. */
	FFAUDIO_ECONNECTION,
};

/** Device type */
enum FFAUDIO_DEV {
	FFAUDIO_DEV_PLAYBACK,
	FFAUDIO_DEV_CAPTURE,
};

/** Device property ID */
enum FFAUDIO_DEV_INFO {
	/** Device ID (API-specific)
	ALSA, PulseAudio, JACK, OSS: NULL-terminated string
	CoreAudio: int*
	DirectSound: GUID*
	WASAPI: wchar_t* */
	FFAUDIO_DEV_ID,

	/** Device name */
	FFAUDIO_DEV_NAME,

	/** Is default device (WASAPI)
	NULL: not default */
	FFAUDIO_DEV_IS_DEFAULT,

	/** Get default format (WASAPI, shared mode)
	Return ffuint[]:  0:format, 1:sample_rate, 2:channels */
	FFAUDIO_DEV_MIX_FORMAT,
};

/** Sample format */
enum FFAUDIO_F {
	FFAUDIO_F_INT8 = 8,
	FFAUDIO_F_UINT8 = 8 | 0x0400,
	FFAUDIO_F_INT16 = 16,
	FFAUDIO_F_INT24 = 24,
	FFAUDIO_F_INT24_4 = 32 | 0x0200,
	FFAUDIO_F_INT32 = 32,
	FFAUDIO_F_FLOAT32 = 32 | 0x0100,
	FFAUDIO_F_FLOAT64 = 64 | 0x0100,
};

enum FFAUDIO_OPEN {
	/** Open playback device */
	FFAUDIO_PLAYBACK,

	/** Open capture device */
	FFAUDIO_CAPTURE,

	/** Open playback device for capturing what is currently playing in the system (WASAPI) */
	FFAUDIO_LOOPBACK,

	/** Use non-blocking I/O
	ffaudio_write(), ffaudio_drain(), ffaudio_read() won't block but will return 0
	 if the operation can't be completed immediately.
	'ffaudio_conf.on_event()' will be called to notify user */
	FFAUDIO_O_NONBLOCK = 0x10,

	/** Open device in exclusive mode (AAudio, WASAPI) */
	FFAUDIO_O_EXCLUSIVE = 0x20,

	/** Open "hw" device, instead of "plughw" (ALSA) */
	FFAUDIO_O_HWDEV = 0x40,

	/** Return FFAUDIO_ESYNC when underrun/overrun is detected */
	FFAUDIO_O_UNSYNC_NOTIFY = 0x80,

	/** Perfomance mode (AAudio) */
	FFAUDIO_O_POWER_SAVE = 0x0100,
	FFAUDIO_O_LOW_LATENCY = 0x0200,

	/** WASAPI will set 'ffaudio_conf.event_h'
	 and let the user perform the signal-delivering work via signal() */
	FFAUDIO_O_USER_EVENTS = 0x0400,
};

typedef struct ffaudio_init_conf {
	/** Application name for PulseAudio & JACK
	NULL: use default name */
	const char *app_name;

	/** Error message */
	const char *error;
} ffaudio_init_conf;

/** Audio buffer configuration */
typedef struct ffaudio_conf {
	/** Audio format */
	ffuint format; // enum FFAUDIO_F
	ffuint sample_rate;
	ffuint channels;

	/** Application name for PulseAudio & JACK
	NULL: use default name */
	const char *app_name;

	/** Device ID returned by dev_info(FFAUDIO_DEV_ID)
	NULL: use default device */
	const char *device_id;

	/** Audio buffer size
	0: use default size
	On return from open(), this is the actual buffer length from audio subsystem */
	ffuint buffer_length_msec;

	/** In a non-blocking mode AAudio calls this function when:
	* some data becomes available in audio buffer for reading (recording);
	* free space is available in audio buffer for writing (playback).
	WARNING: usually this is just for sending a wakeup signal to the main thread;
	 don't perform I/O inside this function! */
	void (*on_event)(void*);
	void *udata;

#ifdef FF_WIN
	/** For non-blocking exclusive mode WASAPI sets this to a newly created Windows Event object.
	User puts this event into WaitFor...Object()-family functions to receive immediate events.
	The handle is closed by ffaudio_interface.free(). */
	HANDLE event_h;
#endif
} ffaudio_conf;

typedef struct ffaudio_dev ffaudio_dev;
typedef struct ffaudio_buf ffaudio_buf;

typedef struct ffaudio_interface {

	/** Initialize audio subsystem
	No function can be called before init()
	Return
	  * 0: success
	  * !=0: error, conf->error contains error message
	Pulse: connects to server;  creates a separate thread for event handling */
	int (*init)(ffaudio_init_conf *conf);

	/** Uninitialize
	Only init() can be called after uninit()
	Pulse: disconnects from server;  destroys the thread */
	void (*uninit)();


	/** Create audio device listing
	mode: enum FFAUDIO_DEV
	Return device object */
	ffaudio_dev* (*dev_alloc)(ffuint mode);

	/** Free device object */
	void (*dev_free)(ffaudio_dev *d);

	/** Get last error message from dev_next() */
	const char* (*dev_error)(ffaudio_dev *d);

	/** Get next audio device
	Return
	  * 0: got next device; call dev_info() to get its properties
	  * >0: finished (no more devices)
	  * <0: error, call dev_error() to get error message */
	int (*dev_next)(ffaudio_dev *d);

	/** Get device property
	i: enum FFAUDIO_DEV_INFO
	Return value */
	const char* (*dev_info)(ffaudio_dev *d, ffuint i);


	/** Create audio buffer */
	ffaudio_buf* (*alloc)();

	/** Free audio buffer */
	void (*free)(ffaudio_buf *b);

	/** Get last error message */
	const char* (*error)(ffaudio_buf *b);

	/** Open audio buffer
	flags: enum FFAUDIO_OPEN
	Return
	  * 0: success
	  * FFAUDIO_EFORMAT: input format isn't supported;  the supported format is set inside 'conf'
	  * FFAUDIO_ERROR: call error() to get error message */
	int (*open)(ffaudio_buf *b, ffaudio_conf *conf, ffuint flags);

	/** Start/continue streaming
	Return
	  * 0: success
	  * FFAUDIO_ERROR */
	int (*start)(ffaudio_buf *b);

	/** Stop/pause streaming
	Return
	  * 0: success
	  * FFAUDIO_ERROR */
	int (*stop)(ffaudio_buf *b);

	/** Clear bufferred data
	Return
	  * 0: success
	  * FFAUDIO_ERROR */
	int (*clear)(ffaudio_buf *b);

	/** Write data to the audio playback device
	The stream must be opened with open(FFAUDIO_PLAYBACK)
	If audio buffer is full:
	  * the function blocks the thread until it can make progress
	  * or returns 0 (FFAUDIO_O_NONBLOCK)
	data: input data (interleaved, consistent with ffaudio_conf.format and ffaudio_conf.channels)
	len: input data size (bytes)
	Return
	  * number of bytes written
	  * -FFAUDIO_ERROR: Error
	  * -FFAUDIO_EDEV_OFFLINE: Device went offline
	  * -FFAUDIO_ESYNC: Underrun detected (not a fatal error) */
	int (*write)(ffaudio_buf *b, const void *data, ffsize len);

	/** Wait until the playback buffer is empty
	The stream must be opened with open(FFAUDIO_PLAYBACK)
	Return
	  * 1: done
	  * 0: buffer is not yet empty (FFAUDIO_O_NONBLOCK)
	  * -FFAUDIO_ERROR: Error */
	int (*drain)(ffaudio_buf *b);

	/** Read data from audio capture device
	The stream must be opened with open(FFAUDIO_CAPTURE) or open(FFAUDIO_LOOPBACK)
	The previous buffer is released automatically
	If audio buffer is empty:
	  * the function blocks the thread until more data is available
	  * or returns 0 (FFAUDIO_O_NONBLOCK)
	buffer: pointer to the data area available for reading
	Return
	  * Number of bytes read
	  * -FFAUDIO_ERROR: Error
	  * -FFAUDIO_EDEV_OFFLINE: Device went offline
	    WASAPI exclusive mode: program may crash while accessing the buffer because Windows unmaps the memory region before we can Release() the buffer.
	  * -FFAUDIO_ESYNC: Overrun detected (not a fatal error) */
	int (*read)(ffaudio_buf *b, const void **buffer);

	/** WASAPI: user calls this function when 'event_h' signals.
	This is required for ffaudio to keep track on the buffer's filled data. */
	void (*signal)(ffaudio_buf *b);
} ffaudio_interface;

/** API for direct use */
FF_EXTERN const ffaudio_interface ffaaudio;
FF_EXTERN const ffaudio_interface ffalsa;
FF_EXTERN const ffaudio_interface ffpulse;
FF_EXTERN const ffaudio_interface ffjack;
FF_EXTERN const ffaudio_interface ffwasapi;
FF_EXTERN const ffaudio_interface ffdsound;
FF_EXTERN const ffaudio_interface ffcoreaudio;
FF_EXTERN const ffaudio_interface ffoss;

#ifndef FFAUDIO_INTERFACE_DEFAULT_PTR
	#define FFAUDIO_INTERFACE_DEFAULT_PTR  NULL
#endif

/** Get the first available API */
static inline const ffaudio_interface* ffaudio_default_interface()
{
	return FFAUDIO_INTERFACE_DEFAULT_PTR;
}
