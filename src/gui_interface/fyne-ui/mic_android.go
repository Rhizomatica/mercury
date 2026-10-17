//go:build android

package main

// Android grants the microphone at run time, not at install: without it
// AAudio opens the capture stream and delivers silence.  Ask before the
// engine starts, and wait for the answer.

/*
#cgo LDFLAGS: -landroid
#include <jni.h>
#include <stdint.h>

// 0: granted, 1: not granted (asked if ask != 0), -1: no answer from Java.
static int mic_permission(uintptr_t envp, uintptr_t ctxp, int ask)
{
	JNIEnv *env = (JNIEnv *) envp;
	jobject ctx = (jobject) ctxp;
	jclass cls = (*env)->GetObjectClass(env, ctx);
	jmethodID check = (*env)->GetMethodID(env, cls, "checkSelfPermission", "(Ljava/lang/String;)I");
	jmethodID req = (*env)->GetMethodID(env, cls, "requestPermissions", "([Ljava/lang/String;I)V");
	if (check == NULL || req == NULL) {
		(*env)->ExceptionClear(env);
		return -1;
	}
	jstring perm = (*env)->NewStringUTF(env, "android.permission.RECORD_AUDIO");
	int rc = (*env)->CallIntMethod(env, ctx, check, perm) == 0 ? 0 : 1;  // PERMISSION_GRANTED is 0
	if (rc == 1 && ask) {
		jclass str = (*env)->FindClass(env, "java/lang/String");
		jobjectArray perms = (*env)->NewObjectArray(env, 1, str, perm);
		(*env)->CallVoidMethod(env, ctx, req, perms, 1);
		(*env)->DeleteLocalRef(env, perms);
		(*env)->DeleteLocalRef(env, str);
	}
	if ((*env)->ExceptionCheck(env)) {
		(*env)->ExceptionClear(env);
		rc = -1;
	}
	(*env)->DeleteLocalRef(env, perm);
	(*env)->DeleteLocalRef(env, cls);
	return rc;
}
*/
import "C"

import (
	"time"

	"fyne.io/fyne/v2/driver"
)

func micPermission(ask bool) int {
	rc := -1
	_ = driver.RunNative(func(ctx any) error {
		if ac, ok := ctx.(*driver.AndroidContext); ok {
			a := C.int(0)
			if ask {
				a = 1
			}
			rc = int(C.mic_permission(C.uintptr_t(ac.Env), C.uintptr_t(ac.Ctx), a))
		}
		return nil
	})
	return rc
}

// ensureMicPermission asks for the microphone and waits up to two minutes
// for the operator's answer.  Without it the engine still runs (it can
// transmit), so a refusal is reported, not fatal.
func ensureMicPermission(logf func(string)) {
	switch micPermission(true) {
	case 0:
		return
	case -1:
		logf("Could not ask Android for the microphone\n")
		return
	}
	logf("Waiting for microphone permission...\n")
	for i := 0; i < 240; i++ {
		time.Sleep(500 * time.Millisecond)
		if micPermission(false) == 0 {
			return
		}
	}
	logf("No microphone permission: Mercury cannot hear the radio (Android Settings > Apps > Mercury > Permissions)\n")
}
