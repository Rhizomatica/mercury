//go:build !mercury_embedded

package main

import "fmt"

func mercuryPrintVersion() {
	fmt.Println("Rhizomatica Mercury UI (engine not embedded)")
}

func mercuryInfoCheck(args []string) bool { return false }

func mercuryStart(defaultConfig, logPath string, args []string) error {
	return fmt.Errorf("mercury engine not embedded (build with -tags mercury_embedded)")
}

func mercuryStop() {}
