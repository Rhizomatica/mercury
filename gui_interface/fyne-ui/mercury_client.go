package main

import (
	"bufio"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
)

// prefMercuryClientPath is the preference key that remembers where the
// operator located the mercury-client executable after the first launch.
const prefMercuryClientPath = "mercuryClientPath"

// launchMercuryClient starts the given Mercury Client binary as a separate
// process so it opens in its own window.
func launchMercuryClient(bin string, log func(string)) error {
	cmd := exec.Command(bin)
	detach(cmd)

	// Forward the child's stdout/stderr into the UI log, so a silent start or a
	// crash is still visible in the application logs.
	if stdout, err := cmd.StdoutPipe(); err == nil {
		go forwardLines(stdout, log)
	}
	if stderr, err := cmd.StderrPipe(); err == nil {
		go forwardLines(stderr, log)
	}

	if err := cmd.Start(); err != nil {
		return fmt.Errorf("starting %s: %w", bin, err)
	}
	// Reap the child when it exits, but never block on it: the Client has its
	// own window and lifecycle.
	go func() { _ = cmd.Wait() }()

	return nil
}

// findMercuryClientBinary resolves the path of the Mercury Client executable.
// Lookup order: the MERCURY_CLIENT env var, the directory of the running UI
// binary (the installer ships them side by side), then PATH.  Every candidate
// is validated as a native executable for this platform, so a stale
// mercury-client.exe on Linux or a wrong-architecture ELF is never returned.
func findMercuryClientBinary() (string, error) {
	name := "mercury-client"
	if runtime.GOOS == "windows" {
		name = "mercury-client.exe"
	}
	if p := os.Getenv("MERCURY_CLIENT"); p != "" && isNativeExecutable(p) {
		return p, nil
	}
	if exe, err := os.Executable(); err == nil {
		if candidate := filepath.Join(filepath.Dir(exe), name); isNativeExecutable(candidate) {
			return candidate, nil
		}
	}
	if p, err := exec.LookPath(name); err == nil && isNativeExecutable(p) {
		return p, nil
	}
	return "", fmt.Errorf("mercury-client binary not found; build it and place it next to this executable or on PATH (or set MERCURY_CLIENT)")
}

// isNativeExecutable reports whether path is a regular file whose executable
// format matches the current OS and architecture.  This guards against stale or
// foreign binaries (e.g. a leftover Windows .exe on Linux) that would otherwise
// fail with an opaque "exec format error" at launch.
func isNativeExecutable(path string) bool {
	if !isRegularFile(path) {
		return false
	}
	f, err := os.Open(path)
	if err != nil {
		return false
	}
	defer f.Close()

	head := make([]byte, 20)
	if _, err := io.ReadFull(f, head); err != nil {
		return false
	}

	switch runtime.GOOS {
	case "windows":
		return head[0] == 'M' && head[1] == 'Z'
	case "darwin":
		// Mach-O: MH_MAGIC, MH_CIGAM, MH_MAGIC_64, MH_CIGAM_64.
		return (head[0] == 0xFE && head[1] == 0xED && head[2] == 0xFA && (head[3] == 0xCE || head[3] == 0xCF)) ||
			(head[0] == 0xCE && head[1] == 0xFA && head[2] == 0xED && head[3] == 0xFE) ||
			(head[0] == 0xCF && head[1] == 0xFA && head[2] == 0xED && head[3] == 0xFE)
	case "linux":
		if head[0] != 0x7F || head[1] != 'E' || head[2] != 'L' || head[3] != 'F' {
			return false
		}
		// e_machine at offset 18, little-endian 2 bytes.
		machine := uint16(head[18]) | uint16(head[19])<<8
		return machine == elfMachine()
	}
	return true
}

// elfMachine maps runtime.GOARCH to its ELF e_machine value.
func elfMachine() uint16 {
	switch runtime.GOARCH {
	case "386":
		return 3 // EM_386
	case "arm":
		return 40 // EM_ARM
	case "arm64":
		return 183 // EM_AARCH64
	default:
		return 62 // EM_X86_64
	}
}

func isRegularFile(path string) bool {
	info, err := os.Stat(path)
	return err == nil && !info.IsDir()
}

// forwardLines feeds every line read from r to log. Used to surface a launched
// Mercury Client's stdout/stderr in the application log.
func forwardLines(r io.Reader, log func(string)) {
	sc := bufio.NewScanner(r)
	for sc.Scan() {
		log(sc.Text() + "\n")
	}
}
