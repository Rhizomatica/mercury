# ffaudio

This directory was copied from: https://github.com/stsaz/ffaudio

Original README follows.

ffaudio is a fast cross-platform interface for Audio Input/Output for C and C++.

It provides advanced features for complex apps like [phiola audio player/recorder](https://github.com/stsaz/phiola), or it can be used by tiny programs like [wav player example](./wav-player-example/player.c).

Contents:

* [Features](#features)
* [How to use](#how-to-use)
	* List all available playback devices
	* Record data from audio device
* [How to build](#how-to-build)
	* Makefile helper
	* Build information for each audio API
* [How to test](#how-to-test)

## Features

* List available playback/capture devices
* Play audio
* Capture audio
* Blocking or non-blocking behaviour for write/drain/read functions
* The most simple API as it can be

Supports:

* AAudio (Android>=26)
	* formats: int16/float
	* mode: shared/exclusive
	* perf mode: power-save/low-latency
	* use "High Priority Callback"
* ALSA (Linux):
	* "hw" and "plughw" modes
* CoreAudio (macOS)
* DirectSound (Windows)
* JACK (Linux)
* OSS (FreeBSD)
* PulseAudio (Linux)
* WASAPI (Windows):
	* shared and exclusive modes
	* loopback mode (record what you hear)

Note: JACK playback is not implemented.


## How to use

Write your cross-platform code using `ffaudio_interface` interface.

### List all available playback devices

```c
	#include <ffaudio/audio.h>

	// get API
	const ffaudio_interface *audio = ffaudio_default_interface();

	// initialize audio subsystem
	ffaudio_init_conf initconf = {};
	int r = audio->init(&initconf);
	if (r < 0)
		exit(1);

	// enumerate devices one by one
	ffaudio_dev *d = audio->dev_alloc(FFAUDIO_DEV_PLAYBACK);
	for (;;) {
		int r = audio->dev_next(d);
		if (r > 0) {
			break;
		} else if (r < 0) {
			printf("error: %s\n", audio->dev_error(d));
			break;
		}

		printf("device: name: '%s'\n", audio->dev_info(d, FFAUDIO_DEV_NAME));
	}
	audio->dev_free(d);

	audio->uninit();
```

### Record data from audio device

```c
	#include <ffaudio/audio.h>

	// get API
	const ffaudio_interface *audio = ffaudio_default_interface();

	// initialize audio subsystem
	ffaudio_init_conf initconf = {};
	int r = audio->init(&initconf);
	if (r < 0)
		exit(1);

	// create audio buffer
	ffaudio_buf *buf = audio->alloc();

	// open audio buffer for recording
	ffaudio_conf bufconf = {};
	bufconf.format = FFAUDIO_F_INT16;
	bufconf.sample_rate = 44100;
	bufconf.channels = 2;
	r = audio->open(buf, &bufconf, FFAUDIO_CAPTURE);
	if (r == FFAUDIO_EFORMAT)
		r = audio->open(buf, &bufconf, FFAUDIO_CAPTURE); // open with the supported format
	if (r < 0)
		exit(1);

	// read data from audio buffer and write it to stderr
	for (;;) {
		const void *data;
		r = audio->read(buf, &data);
		if (r < 0)
			exit(1);

		write(2, data.ptr, data.len);
	}

	audio->free(buf);
	audio->uninit();
```

In order for `ffaudio_default_interface()` function to work you should define `FFAUDIO_INTERFACE_DEFAULT_PTR` with the API you want to use, either in code:

	#define FFAUDIO_INTERFACE_DEFAULT_PTR  &ffalsa
	#include <ffaudio/audio.h>

or in Makefile:

	CFLAGS += -DFFAUDIO_INTERFACE_DEFAULT_PTR="&ffalsa"

But instead of calling `ffaudio_default_interface()` you can just use an API directly, e.g. `ffalsa`.


## How to build

Include the necessary ffaudio C files into your project's build script, e.g.:

```makefile
	./ffaudio-alsa.o: $(FFAUDIO_DIR)/ffaudio/alsa.c $(FFAUDIO_DIR)/ffaudio/audio.h
		gcc -c $(CFLAGS) $< -o $@
```

Use the neceessary linker flags for the audio API, described below.


### Build information for each audio API

Configure your building script to compile particular C file and use appropriate link flags.

| API | Package Dependency | Compile | Linker Flags |
| --- | --- | --- | --- |
| AAudio      | - | `ffaudio/aaudio.c` | `-laaudio` |
| ALSA        | `libalsa-devel` | `ffaudio/alsa.c` | `-lasound` |
| PulseAudio  | `libpulse-devel` | `ffaudio/pulse.c` | `-lpulse` |
| JACK        | `jack-audio-connection-kit-devel` | `ffaudio/jack.c` | `-ljack` |
| WASAPI      | - | `ffaudio/wasapi.c` | `-lole32` |
| DirectSound | - | `ffaudio/dsound.c` | `-ldsound -ldxguid` |
| CoreAudio   | - | `ffaudio/coreaudio.c` | `-framework CoreFoundation -framework CoreAudio` |
| OSS         | - | `ffaudio/oss.c` | `-lm` |


## How to test

```sh
git clone https://github.com/stsaz/ffbase
git clone https://github.com/stsaz/ffaudio
cd ffaudio/test
```

* Linux:

```sh
make -B FFAUDIO_API=alsa
./ffaudio-alsa list
./ffaudio-alsa record 2>file.raw
./ffaudio-alsa play <file.raw

make -B FFAUDIO_API=pulse
./ffaudio-pulse list
./ffaudio-pulse record 2>file.raw
./ffaudio-pulse play <file.raw

make -B FFAUDIO_API=jack
./ffaudio-jack list
./ffaudio-jack record 2>file.raw
# [not implemented] ./ffaudio-jack play <file.raw
```

* Android:

Install Android SDK & NDK; then cross-build for Android:

```sh
make FFAUDIO_API=aaudio \
	SYS=android \
	ROOT_DIR=../.. \
	SDK_DIR=/home/USER/Android/Sdk \
	NDK_VER=YOUR_NDK_VERSION
```

Run the executable on Android:

```sh
./ffaudio-aaudio record 2>file.raw
./ffaudio-aaudio play <file.raw
```

* Windows:

```
make -B FFAUDIO_API=wasapi
.\ffaudio-wasapi.exe list
.\ffaudio-wasapi.exe record 2>file.raw
.\ffaudio-wasapi.exe play <file.raw

make -B FFAUDIO_API=dsound
.\ffaudio-dsound.exe list
.\ffaudio-dsound.exe record 2>file.raw
.\ffaudio-dsound.exe play <file.raw
```

* macOS:

```sh
make FFAUDIO_API=coreaudio
./ffaudio-coreaudio list
./ffaudio-coreaudio record 2>file.raw
./ffaudio-coreaudio play <file.raw
```

* FreeBSD:

```sh
make FFAUDIO_API=oss
./ffaudio-oss list
./ffaudio-oss record 2>file.raw
./ffaudio-oss play <file.raw
```

There are more additional arguments that you can pass to these executable files.


## License

ffaudio is in the public-domain.
