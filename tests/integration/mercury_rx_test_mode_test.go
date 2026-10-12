package integration

import (
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"syscall"
	"testing"
	"time"
)

// `mercury -r` (RX test mode) must decode what `mercury -t` sends, show each
// frame as it decodes, and stop when asked -- in MFSK too.
//
// It did none of these on a bench check of the MFSK carrier move.  The RX path
// it uses dropped every frame decoded while the backend's sync flag was clear,
// and an MFSK frame clears it at the very call that decodes it ("a burst is
// still arriving"), so not one MFSK frame got through, even 1.9.18 to 1.9.18.
// A blocking read of the capture ring on the main thread hung the process on
// SIGINT, in every mode.  And redirected stdout, fully buffered, lost every
// frame of a test that had to be killed.
func TestMercuryRxTestDecodesMFSKAndExits(t *testing.T) {
	repoRoot := mustRepoRoot(t)
	bin := locateOrBuildMercury(t, repoRoot)
	dir := t.TempDir()

	air := filepath.Join(dir, "air.s32le.fifo")       // TX playback -> RX capture
	txCap := filepath.Join(dir, "txcap.s32le.fifo")   // the TX side hears nothing
	rxPlay := filepath.Join(dir, "rxplay.s32le.fifo") // the RX side's playback, drained
	for _, p := range []string{air, txCap, rxPlay} {
		if err := syscall.Mkfifo(p, 0600); err != nil {
			t.Fatalf("mkfifo %s: %v", p, err)
		}
	}
	go func() {
		f, err := os.Open(rxPlay)
		if err == nil {
			_, _ = io.Copy(io.Discard, f)
			f.Close()
		}
	}()

	start := func(name string, args ...string) (*exec.Cmd, string) {
		logPath := filepath.Join(dir, name+".log")
		logf, err := os.Create(logPath)
		if err != nil {
			t.Fatalf("create %s: %v", logPath, err)
		}
		t.Cleanup(func() { logf.Close() })
		args = append(args, "-m", "11", "-x", "fifo",
			"-p", fmt.Sprint(freePortPair(t)), "-b", fmt.Sprint(freePort(t)),
			"-C", filepath.Join(dir, "none.ini"))
		cmd := exec.Command(bin, args...)
		cmd.Dir = dir
		cmd.Env = append(cmd.Environ(), "XDG_STATE_HOME="+dir)
		cmd.Stdout, cmd.Stderr = logf, logf // a file, as an operator redirects it
		if err := cmd.Start(); err != nil {
			t.Fatalf("start mercury %s: %v", name, err)
		}
		t.Cleanup(func() {
			if cmd.ProcessState == nil {
				_ = cmd.Process.Kill()
				_ = cmd.Wait()
			}
		})
		return cmd, logPath
	}

	rx, rxLog := start("rx", "-r", "-i", air, "-o", rxPlay)
	time.Sleep(time.Second)
	tx, _ := start("tx", "-t", "-i", txCap, "-o", air)

	// An MFSK frame is a 13.5 s keydown.  Its decode must reach the file while
	// the receiver is still running.
	deadline := time.Now().Add(45 * time.Second)
	frames := 0
	for time.Now().Before(deadline) && frames == 0 {
		time.Sleep(time.Second)
		b, _ := os.ReadFile(rxLog)
		frames = strings.Count(string(b), "\nFrame ")
	}
	_ = tx.Process.Signal(syscall.SIGINT)
	if _, ok := waitExit(tx, 20*time.Second); !ok {
		t.Errorf("mercury -t still running 20 s after SIGINT")
	}
	if frames == 0 {
		b, _ := os.ReadFile(rxLog)
		t.Fatalf("mercury -r showed no MFSK frame within 45 s:\n%s", b)
	}

	_ = rx.Process.Signal(syscall.SIGINT)
	code, ok := waitExit(rx, 10*time.Second)
	b, _ := os.ReadFile(rxLog)
	if !ok {
		t.Fatalf("mercury -r still running 10 s after SIGINT:\n%s", b)
	}
	if code != 0 || !strings.Contains(string(b), "Test mode stopped") {
		t.Fatalf("mercury -r did not stop in order (exit %d):\n%s", code, b)
	}
}
