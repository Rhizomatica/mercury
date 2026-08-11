package main

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"testing"
	"time"
)

// buildFakeBinary compiles a tiny Go program and writes it to dir/name.  The
// binary is a real native executable — not a script — so isNativeExecutable
// accepts it.  mainBody becomes the program's body (e.g. `func main() {}`).
func buildFakeBinary(t *testing.T, dir, name, mainBody string) string {
	t.Helper()
	src := filepath.Join(dir, "fake_main.go")
	if err := os.WriteFile(src, []byte("package main\n"+mainBody), 0o644); err != nil {
		t.Fatal(err)
	}
	bin := filepath.Join(dir, name)
	goBin, err := exec.LookPath("go")
	if err != nil {
		t.Skip("go not found on PATH")
	}
	cmd := exec.Command(goBin, "build", "-o", bin, src)
	if out, err := cmd.CombinedOutput(); err != nil {
		t.Fatalf("go build: %v\n%s", err, out)
	}
	return bin
}

// TestLaunchMercuryClientRunsBinary exercises the full launch path on Unix: a
// fake native mercury-client binary is resolved through discovery, spawned
// detached, and its stdout is forwarded to the log callback while it runs.
func TestLaunchMercuryClientRunsBinary(t *testing.T) {
	if runtime.GOOS == "windows" {
		t.Skip("lo unch test uses Unix detach")
	}
	dir := t.TempDir()
	marker := filepath.Join(dir, "launched.marker")
	mainBody := fmt.Sprintf(
		`import "os"; func main() { os.WriteFile(%q, []byte("ok"), 0o644); os.Stdout.WriteString("hello from client\n") }`,
		marker,
	)
	bin := buildFakeBinary(t, dir, "mercury-client", mainBody)
	t.Setenv("MERCURY_CLIENT", bin)
	t.Setenv("PATH", os.Getenv("PATH")+string(os.PathListSeparator)+dir)

	var lines []string
	got, err := findMercuryClientBinary()
	if err != nil {
		t.Fatal(err)
	}
	if got != bin {
		t.Fatalf("expected %q, got %q", bin, got)
	}
	if err := launchMercuryClient(bin, func(s string) { lines = append(lines, s) }); err != nil {
		t.Fatal(err)
	}

	deadline := time.Now().Add(5 * time.Second)
	for time.Now().Before(deadline) {
		if _, err := os.Stat(marker); err == nil {
			break
		}
		time.Sleep(50 * time.Millisecond)
	}
	if _, err := os.Stat(marker); err != nil {
		t.Fatalf("child did not run; log=%v", lines)
	}
	if len(lines) != 1 || lines[0] != "hello from client\n" {
		t.Fatalf("expected forwarded stdout [hello from client\\n], got %v", lines)
	}
}
