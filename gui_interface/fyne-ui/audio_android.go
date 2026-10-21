//go:build android

package main

// AAudio plays and records on Android's default route unless told a device,
// and lists none itself: a radio's USB codec, plugged in, may or may not get
// the route (the phone's own microphone often keeps it).  So the UI asks
// Java's AudioManager for the devices and registers them with the engine
// (mercury_ui_set_android_audio_devices); the soundcard lists then offer
// each as "dev:<id>", which aaudio.c opens with AAudioStreamBuilder_setDeviceId.

/*
#include <jni.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "mercury_bridge.h"

// One line per device, "id\tin|out\ttype\tname".  Count, or -1.
static int audio_devices(uintptr_t envp, uintptr_t ctxp, char *out, int len)
{
	JNIEnv *env = (JNIEnv *) envp;
	jobject ctx = (jobject) ctxp;
	jclass cc = (*env)->GetObjectClass(env, ctx);
	jmethodID get = (*env)->GetMethodID(env, cc, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;");
	(*env)->DeleteLocalRef(env, cc);
	jstring svc = (*env)->NewStringUTF(env, "audio");
	jobject am = (*env)->CallObjectMethod(env, ctx, get, svc);
	(*env)->DeleteLocalRef(env, svc);
	if (am == NULL || (*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); return -1; }
	jclass amc = (*env)->GetObjectClass(env, am);
	jmethodID devs = (*env)->GetMethodID(env, amc, "getDevices", "(I)[Landroid/media/AudioDeviceInfo;");
	jobjectArray arr = (jobjectArray) (*env)->CallObjectMethod(env, am, devs, 3);  // GET_DEVICES_ALL
	(*env)->DeleteLocalRef(env, amc);
	(*env)->DeleteLocalRef(env, am);
	if (arr == NULL || (*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); return -1; }
	int n = (*env)->GetArrayLength(env, arr), used = 0;
	out[0] = '\0';
	for (int i = 0; i < n; i++) {
		jobject d = (*env)->GetObjectArrayElement(env, arr, i);
		jclass dc = (*env)->GetObjectClass(env, d);
		int id = (*env)->CallIntMethod(env, d, (*env)->GetMethodID(env, dc, "getId", "()I"));
		int type = (*env)->CallIntMethod(env, d, (*env)->GetMethodID(env, dc, "getType", "()I"));
		int src = (*env)->CallBooleanMethod(env, d, (*env)->GetMethodID(env, dc, "isSource", "()Z"));
		jobject cs = (*env)->CallObjectMethod(env, d, (*env)->GetMethodID(env, dc, "getProductName", "()Ljava/lang/CharSequence;"));
		char name[128] = "";
		if (cs != NULL) {
			jclass csc = (*env)->GetObjectClass(env, cs);
			jstring s = (jstring) (*env)->CallObjectMethod(env, cs, (*env)->GetMethodID(env, csc, "toString", "()Ljava/lang/String;"));
			const char *c = s ? (*env)->GetStringUTFChars(env, s, NULL) : NULL;
			if (c) { snprintf(name, sizeof(name), "%s", c); (*env)->ReleaseStringUTFChars(env, s, c); }
			if (s) (*env)->DeleteLocalRef(env, s);
			(*env)->DeleteLocalRef(env, csc);
			(*env)->DeleteLocalRef(env, cs);
		}
		if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); }
		else {
			int w = snprintf(out + used, (size_t) (len - used), "%d\t%s\t%d\t%s\n", id, src ? "in" : "out", type, name);
			if (w > 0 && used + w < len) used += w;
		}
		(*env)->DeleteLocalRef(env, dc);
		(*env)->DeleteLocalRef(env, d);
	}
	(*env)->DeleteLocalRef(env, arr);
	return n;
}
*/
import "C"

import (
	"fmt"
	"strconv"
	"strings"
	"unsafe"
)

// AudioDeviceInfo types worth offering for a radio, and what to call them.
// Telephony, remote submix, echo reference and the like are left out.
var androidAudioTypes = map[int]string{
	1: "earpiece", 2: "speaker", 3: "wired headset", 4: "wired headphones",
	5: "line, analog", 6: "line, digital", 7: "Bluetooth SCO", 8: "Bluetooth A2DP",
	9: "HDMI", 11: "USB", 12: "USB accessory", 15: "built-in mic", 19: "aux line",
	22: "USB headset", 26: "Bluetooth LE", 27: "Bluetooth LE",
}

// refreshAndroidAudioDevices registers Java's current devices with the
// engine: before it starts, and whenever the soundcard lists are read (a
// radio plugged in later shows up).
func refreshAndroidAudioDevices() {
	buf := make([]byte, 16384)
	n := -1
	android(func(env, ctx C.uintptr_t) {
		n = int(C.audio_devices(env, ctx, (*C.char)(unsafe.Pointer(&buf[0])), C.int(len(buf))))
	})
	if n < 0 {
		return
	}
	var list strings.Builder
	for _, line := range strings.Split(C.GoString((*C.char)(unsafe.Pointer(&buf[0]))), "\n") {
		f := strings.SplitN(line, "\t", 4)
		if len(f) != 4 {
			continue
		}
		t, _ := strconv.Atoi(f[2])
		kind, ok := androidAudioTypes[t]
		if !ok {
			continue
		}
		name := f[3]
		if name == "" {
			name = "Android"
		}
		fmt.Fprintf(&list, "%s\t%s\t%s (%s)\n", f[0], f[1], name, kind)
	}
	cs := C.CString(list.String())
	C.mercury_ui_set_android_audio_devices(cs)
	C.free(unsafe.Pointer(cs))
}
