//go:build android

package main

// An Android app opens no /dev/ttyUSB*: it asks UsbManager for a device,
// the operator grants it, and the app gets a usbfs descriptor.  Hamlib
// drives the rig's USB-serial chip on that descriptor itself ("usbfd:N",
// radio_io/hamlib-android).  The configuration keeps a stable name,
// "usb:VVVV:PPPP[:SERIAL]"; the descriptor is registered with the engine
// (mercury_ui_set_usb_fd), which hands it to Hamlib for that name.

/*
#include <jni.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mercury_bridge.h"

static jobject usb_manager(JNIEnv *env, jobject ctx)
{
	jclass cls = (*env)->GetObjectClass(env, ctx);
	jmethodID get = (*env)->GetMethodID(env, cls, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;");
	(*env)->DeleteLocalRef(env, cls);
	if (get == NULL) { (*env)->ExceptionClear(env); return NULL; }
	jstring name = (*env)->NewStringUTF(env, "usb");
	jobject um = (*env)->CallObjectMethod(env, ctx, get, name);
	(*env)->DeleteLocalRef(env, name);
	if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); return NULL; }
	return um;
}

// The UsbDevice objects, as a Java Object[] (NULL on error).
static jobjectArray usb_devices(JNIEnv *env, jobject um)
{
	jclass cls = (*env)->GetObjectClass(env, um);
	jmethodID list = (*env)->GetMethodID(env, cls, "getDeviceList", "()Ljava/util/HashMap;");
	(*env)->DeleteLocalRef(env, cls);
	if (list == NULL) { (*env)->ExceptionClear(env); return NULL; }
	jobject map = (*env)->CallObjectMethod(env, um, list);
	if (map == NULL || (*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); return NULL; }
	jclass hm = (*env)->GetObjectClass(env, map);
	jmethodID values = (*env)->GetMethodID(env, hm, "values", "()Ljava/util/Collection;");
	jobject col = (*env)->CallObjectMethod(env, map, values);
	jclass cc = (*env)->GetObjectClass(env, col);
	jmethodID toArray = (*env)->GetMethodID(env, cc, "toArray", "()[Ljava/lang/Object;");
	jobjectArray arr = (jobjectArray) (*env)->CallObjectMethod(env, col, toArray);
	(*env)->DeleteLocalRef(env, cc);
	(*env)->DeleteLocalRef(env, col);
	(*env)->DeleteLocalRef(env, hm);
	(*env)->DeleteLocalRef(env, map);
	if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); return NULL; }
	return arr;
}

static void call_string(JNIEnv *env, jobject obj, const char *method, char *out, int len)
{
	out[0] = '\0';
	jclass cls = (*env)->GetObjectClass(env, obj);
	jmethodID m = (*env)->GetMethodID(env, cls, method, "()Ljava/lang/String;");
	(*env)->DeleteLocalRef(env, cls);
	if (m == NULL) { (*env)->ExceptionClear(env); return; }
	jstring s = (jstring) (*env)->CallObjectMethod(env, obj, m);
	// getSerialNumber() throws SecurityException without the permission
	if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); return; }
	if (s == NULL) return;
	const char *c = (*env)->GetStringUTFChars(env, s, NULL);
	if (c) { snprintf(out, (size_t) len, "%s", c); (*env)->ReleaseStringUTFChars(env, s, c); }
	(*env)->DeleteLocalRef(env, s);
}

static int call_int(JNIEnv *env, jobject obj, const char *method)
{
	jclass cls = (*env)->GetObjectClass(env, obj);
	jmethodID m = (*env)->GetMethodID(env, cls, method, "()I");
	(*env)->DeleteLocalRef(env, cls);
	if (m == NULL) { (*env)->ExceptionClear(env); return -1; }
	int v = (*env)->CallIntMethod(env, obj, m);
	if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); return -1; }
	return v;
}

// One line per device: "name\tvid\tpid\tproduct\tserial\n".  Count, or -1.
static int usb_list(uintptr_t envp, uintptr_t ctxp, char *out, int len)
{
	JNIEnv *env = (JNIEnv *) envp;
	jobject um = usb_manager(env, (jobject) ctxp);
	if (um == NULL) return -1;
	jobjectArray arr = usb_devices(env, um);
	(*env)->DeleteLocalRef(env, um);
	if (arr == NULL) return -1;
	int n = (*env)->GetArrayLength(env, arr), used = 0;
	out[0] = '\0';
	for (int i = 0; i < n; i++) {
		jobject d = (*env)->GetObjectArrayElement(env, arr, i);
		char name[128], product[128], serial[128];
		call_string(env, d, "getDeviceName", name, sizeof(name));
		call_string(env, d, "getProductName", product, sizeof(product));
		call_string(env, d, "getSerialNumber", serial, sizeof(serial));
		int w = snprintf(out + used, (size_t) (len - used), "%s\t%04x\t%04x\t%s\t%s\n", name,
		                 call_int(env, d, "getVendorId") & 0xffff, call_int(env, d, "getProductId") & 0xffff,
		                 product, serial);
		if (w > 0 && used + w < len) used += w;
		(*env)->DeleteLocalRef(env, d);
	}
	(*env)->DeleteLocalRef(env, arr);
	return n;
}

static jobject find_device(JNIEnv *env, jobject um, const char *want)
{
	jobjectArray arr = usb_devices(env, um);
	if (arr == NULL) return NULL;
	jobject hit = NULL;
	int n = (*env)->GetArrayLength(env, arr);
	for (int i = 0; i < n && hit == NULL; i++) {
		jobject d = (*env)->GetObjectArrayElement(env, arr, i);
		char name[128];
		call_string(env, d, "getDeviceName", name, sizeof(name));
		if (strcmp(name, want) == 0) hit = d;
		else (*env)->DeleteLocalRef(env, d);
	}
	(*env)->DeleteLocalRef(env, arr);
	return hit;
}

// 0: granted, 1: not yet (asked if ask), -1: error.
static int usb_permission(uintptr_t envp, uintptr_t ctxp, const char *devname, int ask)
{
	JNIEnv *env = (JNIEnv *) envp;
	jobject ctx = (jobject) ctxp;
	jobject um = usb_manager(env, ctx);
	if (um == NULL) return -1;
	jobject dev = find_device(env, um, devname);
	int rc = -1;
	if (dev != NULL) {
		jclass umc = (*env)->GetObjectClass(env, um);
		jmethodID has = (*env)->GetMethodID(env, umc, "hasPermission", "(Landroid/hardware/usb/UsbDevice;)Z");
		rc = (*env)->CallBooleanMethod(env, um, has, dev) ? 0 : 1;
		if (rc == 1 && ask) {
			// An explicit, immutable broadcast nobody listens to: the dialog is
			// what we want; the answer is read back with hasPermission().
			jclass ic = (*env)->FindClass(env, "android/content/Intent");
			jmethodID init = (*env)->GetMethodID(env, ic, "<init>", "(Ljava/lang/String;)V");
			jstring action = (*env)->NewStringUTF(env, "org.rhizomatica.mercury.USB_PERMISSION");
			jobject intent = (*env)->NewObject(env, ic, init, action);
			jclass cc = (*env)->GetObjectClass(env, ctx);
			jmethodID pkg = (*env)->GetMethodID(env, cc, "getPackageName", "()Ljava/lang/String;");
			jobject pname = (*env)->CallObjectMethod(env, ctx, pkg);
			jmethodID setp = (*env)->GetMethodID(env, ic, "setPackage", "(Ljava/lang/String;)Landroid/content/Intent;");
			jobject r = (*env)->CallObjectMethod(env, intent, setp, pname);
			if (r) (*env)->DeleteLocalRef(env, r);
			jclass pic = (*env)->FindClass(env, "android/app/PendingIntent");
			jmethodID gb = (*env)->GetStaticMethodID(env, pic, "getBroadcast",
				"(Landroid/content/Context;ILandroid/content/Intent;I)Landroid/app/PendingIntent;");
			jobject pi = (*env)->CallStaticObjectMethod(env, pic, gb, ctx, 0, intent, 0x04000000);  // FLAG_IMMUTABLE
			jmethodID req = (*env)->GetMethodID(env, umc, "requestPermission",
				"(Landroid/hardware/usb/UsbDevice;Landroid/app/PendingIntent;)V");
			if (pi) (*env)->CallVoidMethod(env, um, req, dev, pi);
			if (pi) (*env)->DeleteLocalRef(env, pi);
			(*env)->DeleteLocalRef(env, pic);
			(*env)->DeleteLocalRef(env, pname);
			(*env)->DeleteLocalRef(env, cc);
			(*env)->DeleteLocalRef(env, intent);
			(*env)->DeleteLocalRef(env, action);
			(*env)->DeleteLocalRef(env, ic);
		}
		(*env)->DeleteLocalRef(env, umc);
		(*env)->DeleteLocalRef(env, dev);
	}
	if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); rc = -1; }
	(*env)->DeleteLocalRef(env, um);
	return rc;
}

// The open connection: held so its descriptor stays valid.
static jobject g_conn;

// The usbfs descriptor of the device, its serial number into serial; -1 on
// error.  Needs the permission.
static int usb_open(uintptr_t envp, uintptr_t ctxp, const char *devname, char *serial, int slen)
{
	JNIEnv *env = (JNIEnv *) envp;
	serial[0] = '\0';
	jobject um = usb_manager(env, (jobject) ctxp);
	if (um == NULL) return -1;
	jobject dev = find_device(env, um, devname);
	int fd = -1;
	if (dev != NULL) {
		jclass umc = (*env)->GetObjectClass(env, um);
		jmethodID open = (*env)->GetMethodID(env, umc, "openDevice",
			"(Landroid/hardware/usb/UsbDevice;)Landroid/hardware/usb/UsbDeviceConnection;");
		jobject conn = (*env)->CallObjectMethod(env, um, open, dev);
		if (conn != NULL && !(*env)->ExceptionCheck(env)) {
			fd = call_int(env, conn, "getFileDescriptor");
			if (fd >= 0) {
				if (g_conn) {
					jclass k = (*env)->GetObjectClass(env, g_conn);
					jmethodID close = (*env)->GetMethodID(env, k, "close", "()V");
					(*env)->CallVoidMethod(env, g_conn, close);
					(*env)->DeleteLocalRef(env, k);
					(*env)->DeleteGlobalRef(env, g_conn);
				}
				g_conn = (*env)->NewGlobalRef(env, conn);
				call_string(env, dev, "getSerialNumber", serial, slen);
			}
			(*env)->DeleteLocalRef(env, conn);
		}
		(*env)->DeleteLocalRef(env, umc);
		(*env)->DeleteLocalRef(env, dev);
	}
	if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); fd = -1; }
	(*env)->DeleteLocalRef(env, um);
	return fd;
}
*/
import "C"

import (
	"bufio"
	"errors"
	"fmt"
	"os"
	"strings"
	"time"
	"unsafe"

	"fyne.io/fyne/v2"
	"fyne.io/fyne/v2/container"
	"fyne.io/fyne/v2/dialog"
	"fyne.io/fyne/v2/driver"
	"fyne.io/fyne/v2/widget"
)

// directUSBLabel says what this is: not the operating system's serial port
// but Mercury (Hamlib) driving the chip itself.
const directUSBLabel = "Direct USB, no driver (experimental)"

type usbDevice struct {
	name, vid, pid, product, serial string
}

func (d usbDevice) spec() string {
	if d.serial != "" {
		return "usb:" + d.vid + ":" + d.pid + ":" + d.serial
	}
	return "usb:" + d.vid + ":" + d.pid
}

func (d usbDevice) String() string {
	p := d.product
	if p == "" {
		p = "USB device"
	}
	return fmt.Sprintf("%s (%s:%s)", p, d.vid, d.pid)
}

func android(f func(env, ctx C.uintptr_t)) {
	_ = driver.RunNative(func(c any) error {
		if ac, ok := c.(*driver.AndroidContext); ok {
			f(C.uintptr_t(ac.Env), C.uintptr_t(ac.Ctx))
		}
		return nil
	})
}

func usbDevices() []usbDevice {
	buf := make([]byte, 16384)
	n := -1
	android(func(env, ctx C.uintptr_t) {
		n = int(C.usb_list(env, ctx, (*C.char)(unsafe.Pointer(&buf[0])), C.int(len(buf))))
	})
	if n <= 0 {
		return nil
	}
	var out []usbDevice
	for _, line := range strings.Split(C.GoString((*C.char)(unsafe.Pointer(&buf[0]))), "\n") {
		f := strings.Split(line, "\t")
		if len(f) == 5 {
			out = append(out, usbDevice{f[0], f[1], f[2], f[3], f[4]})
		}
	}
	return out
}

// usbOpen asks for the device (waiting up to two minutes for the operator),
// opens it and registers its descriptor under the name as (its own spec when
// empty), which it returns.
func usbOpen(d usbDevice, as string, logf func(string)) (string, error) {
	cname := C.CString(d.name)
	defer C.free(unsafe.Pointer(cname))
	perm := func(ask bool) int {
		rc := -1
		a := C.int(0)
		if ask {
			a = 1
		}
		android(func(env, ctx C.uintptr_t) { rc = int(C.usb_permission(env, ctx, cname, a)) })
		return rc
	}
	switch perm(true) {
	case -1:
		return "", errors.New("Android did not list the device")
	case 1:
		logf(fmt.Sprintf("Waiting for permission to use %s...\n", d))
		granted := false
		for i := 0; i < 240 && !granted; i++ {
			time.Sleep(500 * time.Millisecond)
			granted = perm(false) == 0
		}
		if !granted {
			return "", errors.New("no permission for the device")
		}
	}
	serial := make([]byte, 128)
	fd := -1
	android(func(env, ctx C.uintptr_t) {
		fd = int(C.usb_open(env, ctx, cname, (*C.char)(unsafe.Pointer(&serial[0])), C.int(len(serial))))
	})
	if fd < 0 {
		return "", errors.New("Android could not open the device")
	}
	d.serial = C.GoString((*C.char)(unsafe.Pointer(&serial[0])))
	spec := as
	if spec == "" {
		spec = d.spec()
	}
	cspec := C.CString(spec)
	C.mercury_ui_set_usb_fd(cspec, C.int(fd))
	C.free(unsafe.Pointer(cspec))
	logf(fmt.Sprintf("%s opened as %s\n", d, spec))
	return spec, nil
}

// matches: a configured "usb:VVVV:PPPP[:SERIAL]" names this device.
func (d usbDevice) matches(spec string) bool {
	f := strings.SplitN(strings.TrimPrefix(spec, "usb:"), ":", 3)
	if len(f) < 2 || !strings.EqualFold(f[0], d.vid) || !strings.EqualFold(f[1], d.pid) {
		return false
	}
	return len(f) < 3 || d.serial == "" || f[2] == d.serial
}

// configuredUSBDevice reads [ptt] device from the engine's mercury.ini.
func configuredUSBDevice(config string) string {
	f, err := os.Open(config)
	if err != nil {
		return ""
	}
	defer f.Close()
	section := ""
	sc := bufio.NewScanner(f)
	for sc.Scan() {
		l := strings.TrimSpace(sc.Text())
		if strings.HasPrefix(l, "[") && strings.HasSuffix(l, "]") {
			section = strings.ToLower(strings.Trim(l, "[]"))
			continue
		}
		if k, v, ok := strings.Cut(l, "="); ok && section == "ptt" && strings.TrimSpace(k) == "device" {
			return strings.Trim(strings.TrimSpace(v), "\"")
		}
	}
	return ""
}

// ensureUSBRadio, before the engine starts: a radio configured as a
// "usb:..." device is asked for and opened again (Android grants a device
// for the session, and the descriptor is new each time).
func ensureUSBRadio(config string, logf func(string)) {
	spec := configuredUSBDevice(config)
	if !strings.HasPrefix(spec, "usb:") {
		return
	}
	for _, d := range usbDevices() {
		if d.matches(spec) {
			if _, err := usbOpen(d, spec, logf); err != nil {
				logf(fmt.Sprintf("%s: %v; the radio is not keyed by CAT\n", spec, err))
			}
			return
		}
	}
	logf(fmt.Sprintf("%s is not plugged in; the radio is not keyed by CAT\n", spec))
}

// directUSBRow: the Radio dialog's way to pick a USB radio on Android.
func directUSBRow(w fyne.Window, deviceEntry *widget.Entry, logf func(string)) fyne.CanvasObject {
	btn := widget.NewButton(directUSBLabel+"...", func() {
		devs := usbDevices()
		if len(devs) == 0 {
			dialog.ShowInformation(directUSBLabel, "No USB device is plugged in.", w)
			return
		}
		names := make([]string, len(devs))
		for i, d := range devs {
			names[i] = d.String()
		}
		pick := widget.NewSelect(names, nil)
		warn := widget.NewLabel("Mercury drives the radio's USB-serial chip itself,\n" +
			"without the operating system's driver. CP210x chips only\n" +
			"(Icom IC-7100/7300, Yaesu FT-710, many cables). Experimental.")
		dialog.ShowCustomConfirm(directUSBLabel, "Use it", "Cancel",
			container.NewVBox(warn, pick), func(ok bool) {
				if !ok || pick.SelectedIndex() < 0 {
					return
				}
				d := devs[pick.SelectedIndex()]
				go func() {
					spec, err := usbOpen(d, "", logf)
					if err != nil {
						logf(fmt.Sprintf("%s: %v\n", d, err))
						return
					}
					fyne.Do(func() { deviceEntry.SetText(spec) })
					logf("Press Apply to use it for CAT and PTT.\n")
				}()
			}, w)
	})
	return btn
}
