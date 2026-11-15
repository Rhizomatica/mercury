package integration

import (
	"context"
	"fmt"
	"net"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

// Mercury keys through rigctld (-R 2, NET rigctl) and rigctld restarts: an
// update, a crash, the operator's rig app reconnecting.  Hamlib's socket is
// then dead and every PTT call failed with -RIG_EIO ("Broken pipe") until
// Mercury itself was restarted -- while the TNC still told the host PTT ON.
// A PTT OFF lost that way leaves a real rig keyed.  Mercury now reopens the
// session (radio_io.c, hamlib_reopen) and retries the call.
//
// Needs rigctld (Debian: libhamlib-utils) and a Mercury built with Hamlib;
// skipped otherwise.  The rig is Hamlib's dummy (model 1) with PTT, so no
// radio is involved; audio is the null backend.
func TestHamlibSessionSurvivesRigctldRestart(t *testing.T) {
	rigctld, err := exec.LookPath("rigctld")
	if err != nil {
		t.Skip("rigctld not installed (Debian: libhamlib-utils)")
	}
	repoRoot := mustRepoRoot(t)
	bin := locateOrBuildMercury(t, repoRoot)
	if out, _ := exec.Command(bin, "-K").CombinedOutput(); strings.Contains(string(out), "not compiled in") {
		t.Skip("Mercury built without Hamlib")
	}

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	dir := t.TempDir()
	rigPort := freePort(t)
	port := freePortPair(t)
	logPath := filepath.Join(dir, "mercury.log")

	// rigctld on a fixed port, so a restart is the same server to Mercury.
	var rig *exec.Cmd
	startRig := func() {
		t.Helper()
		f, err := os.OpenFile(filepath.Join(dir, "rigctld.log"), os.O_CREATE|os.O_APPEND|os.O_WRONLY, 0600)
		if err != nil {
			t.Fatal(err)
		}
		defer f.Close()
		rig = exec.CommandContext(ctx, rigctld, "-m", "1", "-P", "RIG", "-T", "127.0.0.1", "-t", fmt.Sprint(rigPort))
		rig.Stdout, rig.Stderr = f, f
		if err := rig.Start(); err != nil {
			t.Fatalf("start rigctld: %v", err)
		}
		for i := 0; i < 50; i++ {
			if c, err := net.DialTimeout("tcp", fmt.Sprintf("127.0.0.1:%d", rigPort), 200*time.Millisecond); err == nil {
				c.Close()
				return
			}
			time.Sleep(100 * time.Millisecond)
		}
		t.Fatal("rigctld did not listen")
	}
	stopRig := func() {
		_ = rig.Process.Kill()
		_ = rig.Wait()
	}
	startRig()
	defer stopRig()

	stdout, stderr := tempLogFilesNamed(t, "rig")
	cmd := exec.CommandContext(ctx, bin,
		"-x", "null",
		"-p", fmt.Sprint(port), "-b", fmt.Sprint(port+100),
		"-m", "1", "-v", // DATAC3
		"-R", "2", "-A", fmt.Sprintf("127.0.0.1:%d", rigPort),
		"-L", logPath,
		"-C", filepath.Join(dir, "missing-mercury.ini"))
	cmd.Dir = repoRoot
	cmd.Stdout, cmd.Stderr = stdout, stderr
	if err := startChild(cmd); err != nil {
		t.Fatalf("start mercury: %v", err)
	}
	proc := waitForProcess(cmd)
	defer func() { _ = stopProcess(t, cmd, proc, stdout.Name(), stderr.Name()) }()

	fail := func(format string, args ...interface{}) {
		t.Helper()
		t.Logf("mercury.log:\n%s", readLog(t, logPath))
		printLogs(t, stdout.Name(), stderr.Name())
		t.Fatalf(format, args...)
	}

	ctl, err := waitForTCP(ctx, "127.0.0.1", port, controlPortTimeout, proc)
	if err != nil {
		fail("control port: %v", err)
	}
	defer ctl.Close()
	go func() { // the host side: keep the PTT ON/OFF lines flowing
		buf := make([]byte, 1024)
		for {
			if _, err := ctl.Read(buf); err != nil {
				return
			}
		}
	}()
	bcast, err := net.DialTimeout("tcp", fmt.Sprintf("127.0.0.1:%d", port+100), 5*time.Second)
	if err != nil {
		fail("broadcast port: %v", err)
	}
	defer bcast.Close()

	count := func(s string) int { return strings.Count(readLog(t, logPath), s) }
	// One broadcast frame keys and unkeys the rig once.
	transmit := func(what string, wantOn, wantOff int) {
		t.Helper()
		f := make([]byte, 126) // DATAC3
		f[0] = bcastDataHeader
		copy(f[1:], what)
		if _, err := bcast.Write(kissEncode(kissCmdModemFrame, f)); err != nil {
			fail("%s: write frame: %v", what, err)
		}
		deadline := time.Now().Add(30 * time.Second)
		for time.Now().Before(deadline) {
			select {
			case <-proc.done:
				fail("%s: mercury exited: %v", what, proc.err)
			default:
			}
			if count("PTT ON via Hamlib") >= wantOn && count("PTT OFF via Hamlib") >= wantOff {
				return
			}
			time.Sleep(100 * time.Millisecond)
		}
		fail("%s: rig keyed %d/%d and released %d/%d times (PTT ON failed %d, PTT OFF failed %d)",
			what, count("PTT ON via Hamlib"), wantOn, count("PTT OFF via Hamlib"), wantOff,
			count("PTT ON failed"), count("PTT OFF failed"))
	}

	transmit("before the restart", 1, 1)

	// rigctld restarts while Mercury is idle: the next key-up finds the
	// session dead, reopens it and keys.
	stopRig()
	startRig()
	transmit("after a rigctld restart", 2, 2)
	if count("Hamlib session reopened") < 1 {
		fail("no reopen logged")
	}

	// rigctld is down when Mercury keys: the reopen fails, Mercury stays up,
	// and once rigctld is back (past the reopen interval) keying resumes.
	stopRig()
	f := make([]byte, 126)
	f[0] = bcastDataHeader
	copy(f[1:], "while rigctld is down")
	if _, err := bcast.Write(kissEncode(kissCmdModemFrame, f)); err != nil {
		fail("write frame: %v", err)
	}
	deadline := time.Now().Add(30 * time.Second)
	for count("Hamlib reopen failed") < 1 && time.Now().Before(deadline) {
		time.Sleep(100 * time.Millisecond)
	}
	if count("Hamlib reopen failed") < 1 {
		fail("keying with rigctld down did not try a reopen")
	}
	// The PTT OFF that follows finds no session; that must be said, not lost.
	for count("PTT OFF not sent") < 1 && time.Now().Before(deadline) {
		time.Sleep(100 * time.Millisecond)
	}
	if count("PTT OFF not sent") < 1 {
		fail("a PTT OFF with no Hamlib session was not logged")
	}
	startRig()
	time.Sleep(4 * time.Second) // HAMLIB_REOPEN_INTERVAL_MS is 3 s
	transmit("after rigctld came back", 3, 3)
	if n := count("Hamlib session reopened"); n < 2 {
		fail("reopened %d times, want 2", n)
	}
	for _, l := range strings.Split(readLog(t, logPath), "\n") {
		if strings.Contains(l, "[radio-io]") &&
			(strings.Contains(l, "Hamlib session") || strings.Contains(l, "reopen") ||
				strings.Contains(l, "failed") || strings.Contains(l, "not sent")) {
			t.Log(strings.TrimSpace(l))
		}
	}
}
