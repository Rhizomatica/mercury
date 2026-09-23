package integration

import (
	"bytes"
	"fmt"
	"os/exec"
	"path/filepath"
	"strings"
	"syscall"
	"testing"
	"time"
)

// startTxTest runs `mercury -t` (TX test mode: back-to-back frames, each its
// own keydown) on the null audio backend, in its own process group, and waits
// until it is mid-frame.  DATAC17 frames last ~7.4 s, so a signal landing now
// has to wait for the frame in flight.
func startTxTest(t *testing.T) (*exec.Cmd, *bytes.Buffer) {
	t.Helper()
	repoRoot := mustRepoRoot(t)
	bin := locateOrBuildMercury(t, repoRoot)
	dir := t.TempDir()
	var out bytes.Buffer
	cmd := exec.Command(bin, "-t", "-x", "null", "-m", "9",
		"-p", fmt.Sprint(freePortPair(t)), "-C", filepath.Join(dir, "none.ini"))
	cmd.Dir = dir
	cmd.Env = append(cmd.Environ(), "XDG_STATE_HOME="+dir)
	cmd.Stdout, cmd.Stderr = &out, &out
	cmd.SysProcAttr = &syscall.SysProcAttr{Setpgid: true}
	if err := cmd.Start(); err != nil {
		t.Fatalf("start mercury -t: %v", err)
	}
	time.Sleep(4 * time.Second)
	return cmd, &out
}

func waitExit(cmd *exec.Cmd, within time.Duration) (int, bool) {
	done := make(chan error, 1)
	go func() { done <- cmd.Wait() }()
	select {
	case <-done:
		return cmd.ProcessState.ExitCode(), true
	case <-time.After(within):
		_ = cmd.Process.Kill()
		<-done
		return -1, false
	}
}

// One shutdown request delivered twice must still be ONE orderly shutdown.
//
// `sudo timeout -s INT 40 mercury -t ...` does exactly that: timeout signals
// the whole process group and sudo relays the signal as well.  Mercury took
// the second copy as "force exit" and _exit()ed mid-frame with PTT asserted;
// on a hermes_shm station the radio daemon held the key, and an IC-7100 stayed
// in transmit for 4 h 40 min.
func TestMercuryTxTestDuplicateSignalShutsDownCleanly(t *testing.T) {
	cmd, out := startTxTest(t)
	pid := cmd.Process.Pid
	_ = syscall.Kill(-pid, syscall.SIGINT) // the process group, as timeout does
	// sudo's relay lands a moment later, as a separate delivery: two signals
	// sent back to back would coalesce into one pending SIGINT.
	time.Sleep(100 * time.Millisecond)
	_ = syscall.Kill(pid, syscall.SIGINT) // relayed, as sudo does

	code, ok := waitExit(cmd, 20*time.Second)
	log := out.String()
	if !ok {
		t.Fatalf("mercury -t still running 20 s after its signal:\n%s", log)
	}
	if strings.Contains(log, "forcing exit") {
		t.Fatalf("a duplicated signal forced the exit (PTT may be left keyed):\n%s", log)
	}
	if code != 0 || !strings.Contains(log, "Shutting down PTT method") {
		t.Fatalf("no orderly shutdown (exit %d):\n%s", code, log)
	}
}

// Someone insisting -- a second signal well after the first -- still forces
// the exit, promptly, but through the path that drops PTT first.
func TestMercuryTxTestForcedExitUnkeysFirst(t *testing.T) {
	cmd, out := startTxTest(t)
	pid := cmd.Process.Pid
	_ = syscall.Kill(pid, syscall.SIGINT)
	time.Sleep(2 * time.Second) // still inside the 7.4 s frame
	_ = syscall.Kill(pid, syscall.SIGINT)

	_, ok := waitExit(cmd, 5*time.Second)
	log := out.String()
	if !ok {
		t.Fatalf("a second signal did not force the exit within 5 s:\n%s", log)
	}
	if !strings.Contains(log, "Transmitter unkeyed; exiting.") {
		t.Fatalf("forced exit did not go through the unkey path:\n%s", log)
	}
}
