//go:build !android

package main

// Only Android asks for the microphone at run time from Go (mic_android.go);
// iOS asks in the audio session, desktops do not ask.
func ensureMicPermission(logf func(string)) {}
