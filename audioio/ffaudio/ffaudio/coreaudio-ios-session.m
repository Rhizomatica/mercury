/** ffaudio: the AVAudioSession side of the iOS CoreAudio backend.

The audio session is Objective-C only, so it lives here and coreaudio-ios.c
calls these three C functions.  An iOS app may configure the session itself
before starting Mercury; activating it again here with the same settings is
harmless.
*/

#import <AVFoundation/AVFoundation.h>
#include <dispatch/dispatch.h>
#include <stdio.h>
#include <string.h>

static void session_err(char *err, size_t cap, const char *what, NSError *e)
{
	if (err == NULL || cap == 0)
		return;
	snprintf(err, cap, "%s: %s", what,
		 e != nil ? e.localizedDescription.UTF8String : "failed");
}

/* Microphone permission.  A capture stream without it delivers silence, which
 * looks to a modem like a dead band rather than a refusal, so say it plainly.
 * When the user has not been asked yet, ask and wait: this runs on Mercury's
 * own thread, never on the main thread, so waiting does not freeze the UI. */
static int session_record_permission(char *err, size_t cap)
{
	AVAudioApplication *app = [AVAudioApplication sharedInstance];
	AVAudioApplicationRecordPermission p = app.recordPermission;
	if (p == AVAudioApplicationRecordPermissionUndetermined) {
		dispatch_semaphore_t done = dispatch_semaphore_create(0);
		__block BOOL granted = NO;
		[AVAudioApplication requestRecordPermissionWithCompletionHandler:^(BOOL ok) {
			granted = ok;
			dispatch_semaphore_signal(done);
		}];
		if (dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 60 * NSEC_PER_SEC)) != 0) {
			snprintf(err, cap, "microphone permission: no answer");
			return -1;
		}
		p = granted ? AVAudioApplicationRecordPermissionGranted
			    : AVAudioApplicationRecordPermissionDenied;
	}
	if (p != AVAudioApplicationRecordPermissionGranted) {
		snprintf(err, cap, "microphone permission denied (Settings > Privacy > Microphone)");
		return -1;
	}
	return 0;
}

/* Activate (activate=1) or release (0) the session.  Play-and-record, in
 * measurement mode: the system's gain control and voice processing would
 * distort a modem signal, and measurement mode turns them off.  48 kHz is asked
 * for, not required: the rate the route actually runs at is returned in *rate,
 * and Mercury resamples from it. */
int ffcoreaudio_ios_session(int activate, double *rate, char *err, size_t cap)
{
	@autoreleasepool {
		AVAudioSession *s = [AVAudioSession sharedInstance];
		NSError *e = nil;

		if (!activate) {
			[s setActive:NO
			 withOptions:AVAudioSessionSetActiveOptionNotifyOthersOnDeactivation
			       error:&e];
			return 0;
		}

		if (![s setCategory:AVAudioSessionCategoryPlayAndRecord
				mode:AVAudioSessionModeMeasurement
			     options:AVAudioSessionCategoryOptionDefaultToSpeaker
			       error:&e]) {
			session_err(err, cap, "AVAudioSession setCategory", e);
			return -1;
		}
		[s setPreferredSampleRate:48000.0 error:nil];
		[s setPreferredIOBufferDuration:0.02 error:nil];
		if (![s setActive:YES error:&e]) {
			session_err(err, cap, "AVAudioSession setActive", e);
			return -1;
		}
		if (session_record_permission(err, cap) != 0)
			return -1;
		if (rate != NULL)
			*rate = s.sampleRate;
		return 0;
	}
}

/* The name of the current input (capture=1) or output port, e.g. "USB Audio
 * CODEC" for a radio interface or "iPhone Microphone".  iOS has no device list
 * to choose from: the route is the device, and it follows what is plugged in. */
int ffcoreaudio_ios_route_name(int capture, char *buf, size_t cap)
{
	@autoreleasepool {
		AVAudioSessionRouteDescription *r = [AVAudioSession sharedInstance].currentRoute;
		NSArray<AVAudioSessionPortDescription *> *ports = capture ? r.inputs : r.outputs;
		const char *name = ports.count ? ports.firstObject.portName.UTF8String : NULL;
		if (name == NULL || cap == 0)
			return -1;
		snprintf(buf, cap, "%s", name);
		return 0;
	}
}
