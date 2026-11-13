//go:build !(hybris && linux)

package main

import "fyne.io/fyne/v2"

// Only Sailfish OS (the hybris build) needs a hand with its on-screen keyboard
// (osk_hybris.go); elsewhere the platform's own text input does it.
func startOnScreenKeyboard(app fyne.App, logf func(string)) {}
