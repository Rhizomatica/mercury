//go:build !mercury_embedded

package main

import "fmt"

func mercuryPrintVersion() {
	fmt.Println("Rhizomatica Mercury UI (engine not embedded)")
}

func mercuryInfoCheck(args []string, defaultConfig string) bool { return false }

func mercuryStart(defaultConfig, logPath string, args []string) error {
	return fmt.Errorf("mercury engine not embedded (build with -tags mercury_embedded)")
}

func mercuryLogStatus() string { return "" }

// A remote UI has no local engine and no local config: an empty path with
// loaded == true makes the caller show nothing (no "not found" dialog).
func mercuryConfigStatus() (string, bool) { return "", true }

func mercuryStop() {}
