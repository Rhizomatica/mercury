package integration

import (
	"fmt"
	"os"
	"runtime"
	"testing"
)

// TestMain gives every Mercury these tests start a private state directory.
//
// Mercury persists ARQ and broadcast chat to a message store under the user's
// state directory ($XDG_STATE_HOME, else ~/.local/state on Linux;
// %LOCALAPPDATA% on Windows; ~/Library/Application Support on macOS).  The
// harness passed its environment straight through, so every test instance
// loaded -- and could append to -- the developer's real chat history.  The
// 2026-09-12 broadcast NNCP trial left RaptorQ frames in it that way.
//
// The children inherit this process's environment (startChild, the channel
// bridge and the sock simulator all build on os.Environ()), so setting it here
// covers every test in the package.
func TestMain(m *testing.M) {
	dir, err := os.MkdirTemp("", "mercury-it-state-")
	if err != nil {
		fmt.Fprintf(os.Stderr, "integration: cannot create a private state dir: %v\n", err)
		os.Exit(1)
	}
	for _, k := range []string{"XDG_STATE_HOME", "LOCALAPPDATA", "APPDATA"} {
		os.Setenv(k, dir)
	}
	if runtime.GOOS == "darwin" {
		// The store has no XDG override on macOS; it is found through HOME.
		os.Setenv("HOME", dir)
	}

	code := m.Run()
	os.RemoveAll(dir)
	os.Exit(code)
}
