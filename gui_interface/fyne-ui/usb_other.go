//go:build !android

package main

import (
	"fyne.io/fyne/v2"
	"fyne.io/fyne/v2/widget"
)

// Only an Android app needs the UI to open a USB radio (usb_android.go).
// Elsewhere a "usb:VVVV:PPPP[:SERIAL]" device path goes to Hamlib as typed.

func ensureUSBRadio(config string, logf func(string)) {}

func directUSBRow(w fyne.Window, deviceEntry *widget.Entry, logf func(string)) fyne.CanvasObject {
	return nil
}
