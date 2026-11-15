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

// rigLab is one Mercury keying a dummy rig through a rigctld that the test
// can stop and start on the same port, plus the broadcast port to make it
// transmit.
type rigLab struct {
	t        *testing.T
	ctx      context.Context
	dir      string
	rigctld  string
	rigPort  int
	logPath  string
	frameLen int
	rig      *exec.Cmd
	proc     *processWait
	bcast    net.Conn
	stdout   string
	stderr   string
}

// newRigLab starts rigctld and Mercury in modem mode `mode` (-m), whose
// broadcast frames carry frameLen bytes.
func newRigLab(t *testing.T, mode, frameLen int) *rigLab {
	t.Helper()
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
	t.Cleanup(cancel)
	dir := t.TempDir()
	l := &rigLab{t: t, ctx: ctx, dir: dir, rigctld: rigctld, rigPort: freePort(t),
		logPath: filepath.Join(dir, "mercury.log"), frameLen: frameLen}
	l.startRig()
	t.Cleanup(l.stopRig)

	port := freePortPair(t)
	stdout, stderr := tempLogFilesNamed(t, "rig")
	l.stdout, l.stderr = stdout.Name(), stderr.Name()
	cmd := exec.CommandContext(ctx, bin,
		"-x", "null",
		"-p", fmt.Sprint(port), "-b", fmt.Sprint(port+100),
		"-m", fmt.Sprint(mode), "-v",
		"-R", "2", "-A", fmt.Sprintf("127.0.0.1:%d", l.rigPort),
		"-L", l.logPath,
		"-C", filepath.Join(dir, "missing-mercury.ini"))
	cmd.Dir = repoRoot
	cmd.Stdout, cmd.Stderr = stdout, stderr
	if err := startChild(cmd); err != nil {
		t.Fatalf("start mercury: %v", err)
	}
	l.proc = waitForProcess(cmd)
	t.Cleanup(func() { _ = stopProcess(t, cmd, l.proc, l.stdout, l.stderr) })

	ctl, err := waitForTCP(ctx, "127.0.0.1", port, controlPortTimeout, l.proc)
	if err != nil {
		l.fail("control port: %v", err)
	}
	t.Cleanup(func() { ctl.Close() })
	go func() { // the host side: keep the PTT ON/OFF lines flowing
		buf := make([]byte, 1024)
		for {
			if _, err := ctl.Read(buf); err != nil {
				return
			}
		}
	}()
	l.bcast, err = net.DialTimeout("tcp", fmt.Sprintf("127.0.0.1:%d", port+100), 5*time.Second)
	if err != nil {
		l.fail("broadcast port: %v", err)
	}
	t.Cleanup(func() { l.bcast.Close() })
	return l
}

// startRig runs rigctld on a fixed port, so a restart is the same server to Mercury.
func (l *rigLab) startRig() {
	l.t.Helper()
	f, err := os.OpenFile(filepath.Join(l.dir, "rigctld.log"), os.O_CREATE|os.O_APPEND|os.O_WRONLY, 0600)
	if err != nil {
		l.t.Fatal(err)
	}
	defer f.Close()
	l.rig = exec.CommandContext(l.ctx, l.rigctld, "-m", "1", "-P", "RIG", "-T", "127.0.0.1", "-t", fmt.Sprint(l.rigPort))
	l.rig.Stdout, l.rig.Stderr = f, f
	if err := l.rig.Start(); err != nil {
		l.t.Fatalf("start rigctld: %v", err)
	}
	for i := 0; i < 50; i++ {
		if c, err := net.DialTimeout("tcp", fmt.Sprintf("127.0.0.1:%d", l.rigPort), 200*time.Millisecond); err == nil {
			c.Close()
			return
		}
		time.Sleep(100 * time.Millisecond)
	}
	l.t.Fatal("rigctld did not listen")
}

func (l *rigLab) stopRig() {
	_ = l.rig.Process.Kill()
	_ = l.rig.Wait()
}

func (l *rigLab) fail(format string, args ...interface{}) {
	l.t.Helper()
	l.t.Logf("mercury.log:\n%s", readLog(l.t, l.logPath))
	printLogs(l.t, l.stdout, l.stderr)
	l.t.Fatalf(format, args...)
}

func (l *rigLab) count(s string) int { return strings.Count(readLog(l.t, l.logPath), s) }

// send queues one broadcast frame: Mercury keys, modulates and unkeys once.
func (l *rigLab) send(what string) {
	l.t.Helper()
	f := make([]byte, l.frameLen)
	f[0] = bcastDataHeader
	copy(f[1:], what)
	if _, err := l.bcast.Write(kissEncode(kissCmdModemFrame, f)); err != nil {
		l.fail("%s: write frame: %v", what, err)
	}
}

// waitFor polls the log until cond holds or 30 s pass.
func (l *rigLab) waitFor(what string, cond func() bool) {
	l.t.Helper()
	deadline := time.Now().Add(30 * time.Second)
	for time.Now().Before(deadline) {
		select {
		case <-l.proc.done:
			l.fail("%s: mercury exited: %v", what, l.proc.err)
		default:
		}
		if cond() {
			return
		}
		time.Sleep(100 * time.Millisecond)
	}
	l.fail("%s: timed out (rig keyed %d, released %d; PTT ON failed %d, PTT OFF failed %d, not sent %d)",
		what, l.count("PTT ON via Hamlib"), l.count("PTT OFF via Hamlib"),
		l.count("PTT ON failed"), l.count("PTT OFF failed"), l.count("not sent"))
}

// transmit sends one frame and waits until the rig was keyed wantOn and
// released wantOff times in all.
func (l *rigLab) transmit(what string, wantOn, wantOff int) {
	l.t.Helper()
	l.send(what)
	l.waitFor(what, func() bool {
		return l.count("PTT ON via Hamlib") >= wantOn && l.count("PTT OFF via Hamlib") >= wantOff
	})
}

func (l *rigLab) logRadioLines() {
	for _, line := range strings.Split(readLog(l.t, l.logPath), "\n") {
		if strings.Contains(line, "[radio-io]") &&
			(strings.Contains(line, "Hamlib session") || strings.Contains(line, "reopen") ||
				strings.Contains(line, "failed") || strings.Contains(line, "not sent") ||
				strings.Contains(line, "Pending")) {
			l.t.Log(strings.TrimSpace(line))
		}
	}
}

func TestHamlibSessionSurvivesRigctldRestart(t *testing.T) {
	l := newRigLab(t, 1, 126) // DATAC3
	defer l.logRadioLines()

	l.transmit("before the restart", 1, 1)

	// rigctld restarts while Mercury is idle: the next key-up finds the
	// session dead, reopens it and keys.
	l.stopRig()
	l.startRig()
	l.transmit("after a rigctld restart", 2, 2)
	if l.count("Hamlib session reopened") < 1 {
		l.fail("no reopen logged")
	}

	// rigctld is down when Mercury keys: the reopen fails and Mercury stays up.
	l.stopRig()
	l.send("while rigctld is down")
	l.waitFor("a reopen with rigctld down", func() bool { return l.count("Hamlib reopen failed") >= 1 })
	// The PTT OFF that follows finds no session; that must be said, not lost.
	l.waitFor("the PTT OFF with no session", func() bool { return l.count("PTT OFF not sent") >= 1 })

	// ...and not forgotten: once rigctld is back, the idle poll reopens the
	// session and releases the rig, with no new transmission.
	offs := l.count("PTT OFF via Hamlib")
	l.startRig()
	l.waitFor("the pending PTT OFF", func() bool { return l.count("Pending PTT OFF delivered") >= 1 })
	if l.count("PTT OFF via Hamlib") <= offs {
		l.fail("the pending PTT OFF was reported delivered but not sent")
	}

	// Keying resumes on the session the poll reopened.
	l.transmit("after rigctld came back", 3, offs+2)
	if n := l.count("Hamlib session reopened"); n < 2 {
		l.fail("reopened %d times, want 2", n)
	}
}

// A key-up that finds rigctld down tries a reopen and fails.  The unkey that
// follows a short frame comes well inside HAMLIB_REOPEN_INTERVAL_MS; it must
// make its own attempt rather than be dropped by the pace that limits
// key-ups, since a late unkey costs a carrier left on air.
func TestHamlibPttOffIsNotPaced(t *testing.T) {
	l := newRigLab(t, 2, 14) // DATAC0: ~1.1 s on air, against the 3 s pace
	defer l.logRadioLines()

	l.transmit("session up", 1, 1)
	l.stopRig()
	l.send("while rigctld is down")
	l.waitFor("the key-up's reopen", func() bool { return l.count("Hamlib reopen failed") >= 1 })
	l.waitFor("the unkey", func() bool { return l.count("PTT OFF not sent")+l.count("PTT OFF failed") >= 1 })
	if n := l.count("Hamlib reopen failed"); n < 2 {
		l.fail("the PTT OFF was dropped by the reopen pace (%d failed reopens, want 2: key-up and unkey)", n)
	}
}
