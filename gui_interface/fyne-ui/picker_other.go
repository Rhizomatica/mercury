//go:build !darwin || ios

package main

// No system panel here: the caller shows Fyne's dialog (picker_macos.go).

func nativePick(folder bool, done func(path string, bookmark []byte)) bool { return false }

func nativeOpenBookmark(bookmark []byte) string { return "" }
