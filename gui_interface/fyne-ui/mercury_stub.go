//go:build !mercury_embedded

package main

import "fmt"

func mercuryStart(configPath, logPath string, verbose bool) error {
	return fmt.Errorf("mercury engine not embedded (build with -tags mercury_embedded)")
}

func mercuryStop() {}
