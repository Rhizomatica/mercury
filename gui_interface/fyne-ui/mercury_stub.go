//go:build !mercury_embedded

package main

import "fmt"

func mercuryInfoCheck(args []string) bool { return false }

func mercuryStart(defaultConfig, logPath string, args []string) error {
	return fmt.Errorf("mercury engine not embedded (build with -tags mercury_embedded)")
}

func mercuryStop() {}
