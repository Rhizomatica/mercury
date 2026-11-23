package integration

import (
	"bufio"
	"context"
	"fmt"
	"net"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"sync/atomic"
	"syscall"
	"testing"
	"time"
)

// A gateway receiving an upload that stalls.  BPQ32 closes a Winlink session
// after about 130 s with no data either way and no BUFFER above 0 (#392), and
// a station that is only receiving has nothing of its own to report.  So
// while the peer's data is still on its way, Mercury pulses BUFFER 1, then
// the true count, once nothing has passed for 50 s.
//
// A uploads to B, and A's audio stops reaching B once B has its first bytes.
// B's host must see a pulse about 50 s later, then another one, each followed
// at once by BUFFER 0.
//
// These two run in real time, about four minutes together, and the suite is
// already close to its 25-minute limit in CI, so they run only with
// MERCURY_TEST_BUFFER_PULSE=1.  The rule itself is unit-tested
// (test_tnc_buffer_pulse_only_for_a_stalled_inbound).
func TestStalledInboundPulsesBuffer(t *testing.T) {
	requireBufferPulseTests(t)
	runBufferPulse(t, true)
}

// A session with nothing on its way gets no pulse: an idle BBS user still
// meets the host's own idle timeout.
func TestIdleSessionDoesNotPulseBuffer(t *testing.T) {
	requireBufferPulseTests(t)
	runBufferPulse(t, false)
}

func requireBufferPulseTests(t *testing.T) {
	if os.Getenv("MERCURY_TEST_BUFFER_PULSE") == "" {
		t.Skip("set MERCURY_TEST_BUFFER_PULSE=1 to run (minutes of real-time audio)")
	}
}

func runBufferPulse(t *testing.T, upload bool) {
	repoRoot := mustRepoRoot(t)
	bin := locateOrBuildMercury(t, repoRoot)

	dir := t.TempDir()
	aRX := filepath.Join(dir, "a_rx.s32le.fifo")
	aTX := filepath.Join(dir, "a_tx.s32le.fifo")
	bRX := filepath.Join(dir, "b_rx.s32le.fifo")
	bTX := filepath.Join(dir, "b_tx.s32le.fifo")
	for _, p := range []string{aRX, aTX, bRX, bTX} {
		if err := syscall.Mkfifo(p, 0600); err != nil {
			t.Fatalf("mkfifo %s: %v", p, err)
		}
	}

	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Minute)
	defer cancel()

	start := func(name, rx, tx string, port int) (*exec.Cmd, *processWait, *os.File, *os.File) {
		stdout, stderr := tempLogFilesNamed(t, name)
		cmd := exec.CommandContext(ctx, bin, "-x", "fifo", "-i", rx, "-o", tx,
			"-p", fmt.Sprint(port), "-b", fmt.Sprint(port+100), "-m", "1",
			"-C", filepath.Join(t.TempDir(), "missing-mercury.ini"))
		cmd.Dir = repoRoot
		cmd.Stdout = stdout
		cmd.Stderr = stderr
		if err := startChild(cmd); err != nil {
			t.Fatalf("start mercury %s: %v", name, err)
		}
		return cmd, waitForProcess(cmd), stdout, stderr
	}
	aPort, bPort := freePortPair(t), freePortPair(t)
	cmdA, procA, outA, errA := start("A", aRX, aTX, aPort)
	defer func() { _ = stopProcess(t, cmdA, procA, outA.Name(), errA.Name()) }()
	cmdB, procB, outB, errB := start("B", bRX, bTX, bPort)
	defer func() { _ = stopProcess(t, cmdB, procB, outB.Name(), errB.Name()) }()
	logs := func() {
		printLogs(t, outA.Name(), errA.Name())
		printLogs(t, outB.Name(), errB.Name())
	}

	var muted atomic.Bool
	go pacedBridge(ctx, aTX, bRX, func() bool { return !muted.Load() })
	go pacedBridge(ctx, bTX, aRX, func() bool { return true })

	connA, err := waitForTCP(ctx, "127.0.0.1", aPort, controlPortTimeout, procA)
	if err != nil {
		logs()
		t.Fatalf("mercury A control port: %v", err)
	}
	defer connA.Close()
	connB, err := waitForTCP(ctx, "127.0.0.1", bPort, controlPortTimeout, procB)
	if err != nil {
		logs()
		t.Fatalf("mercury B control port: %v", err)
	}
	defer connB.Close()
	dataA, err := net.Dial("tcp", fmt.Sprintf("127.0.0.1:%d", aPort+1))
	if err != nil {
		t.Fatalf("mercury A data port: %v", err)
	}
	defer dataA.Close()
	dataB, err := net.Dial("tcp", fmt.Sprintf("127.0.0.1:%d", bPort+1))
	if err != nil {
		t.Fatalf("mercury B data port: %v", err)
	}
	defer dataB.Close()

	rwA := bufio.NewReadWriter(bufio.NewReader(connA), bufio.NewWriter(connA))
	rwB := bufio.NewReadWriter(bufio.NewReader(connB), bufio.NewWriter(connB))
	setup := func(conn net.Conn, rw *bufio.ReadWriter, call string, cmds ...string) {
		for _, c := range append([]string{"MYCALL " + call}, cmds...) {
			if got := sendControlCommand(t, conn, rw, c); !strings.HasPrefix(got, "OK") {
				logs()
				t.Fatalf("%s %q: got %q", call, c, got)
			}
			if strings.HasPrefix(c, "MYCALL") {
				if line, err := rw.ReadString('\r'); err != nil || !strings.HasPrefix(line, "REGISTERED") {
					t.Fatalf("%s after MYCALL: %q, %v", call, line, err)
				}
			}
		}
	}
	setup(connA, rwA, "TESTA", "RETRIES 40")
	setup(connB, rwB, "TESTB", "LISTEN ON")
	_ = connA.SetDeadline(time.Time{})
	_ = connB.SetDeadline(time.Time{})

	// A's control port is read so that it never backs up.
	aConnected := make(chan struct{})
	go func() {
		said := false
		for {
			line, err := rwA.ReadString('\r')
			if err != nil {
				return
			}
			if !said && strings.HasPrefix(line, "CONNECTED") {
				said = true
				close(aConnected)
			}
		}
	}()

	// B's host: the first bytes stop A's audio; the time of the last is kept.
	var lastData atomic.Int64
	go func() {
		buf := make([]byte, 4096)
		for {
			n, err := dataB.Read(buf)
			if n > 0 {
				lastData.Store(time.Now().UnixMilli())
				muted.Store(true)
			}
			if err != nil {
				return
			}
		}
	}()

	type ctlLine struct {
		text string
		at   time.Time
	}
	lines := make(chan ctlLine, 256)
	go func() {
		defer close(lines)
		for {
			line, err := rwB.ReadString('\r')
			if err != nil {
				return
			}
			lines <- ctlLine{strings.TrimSuffix(line, "\r"), time.Now()}
		}
	}()

	if _, err := rwA.WriteString("CONNECT TESTA TESTB\r"); err != nil {
		t.Fatal(err)
	}
	if err := rwA.Flush(); err != nil {
		t.Fatal(err)
	}
	select {
	case <-aConnected:
	case <-time.After(90 * time.Second):
		logs()
		t.Fatal("not connected within 90 s")
	}
	if !upload {
		quiet := time.After(70 * time.Second)
		for {
			select {
			case l, ok := <-lines:
				if !ok {
					logs()
					t.Fatal("B's control port closed")
				}
				if l.text == "BUFFER 1" || strings.HasPrefix(l.text, "DISCONNECTED") {
					logs()
					t.Fatalf("idle session: got %q", l.text)
				}
			case <-quiet:
				return
			}
		}
	}
	if _, err := dataA.Write(make([]byte, 20000)); err != nil {
		t.Fatalf("write upload: %v", err)
	}

	pulses := 0
	var previous time.Time
	deadline := time.After(200 * time.Second)
	for pulses < 2 {
		select {
		case l, ok := <-lines:
			if !ok {
				logs()
				t.Fatal("B's control port closed")
			}
			if strings.HasPrefix(l.text, "DISCONNECTED") {
				logs()
				t.Fatalf("session ended after %d pulses", pulses)
			}
			if l.text != "BUFFER 1" {
				continue
			}
			if !muted.Load() {
				logs()
				t.Fatal("a pulse before the upload stalled")
			}
			since := previous
			if since.IsZero() {
				since = time.UnixMilli(lastData.Load())
			}
			if gap := l.at.Sub(since); gap < 49*time.Second || gap > 56*time.Second {
				logs()
				t.Fatalf("pulse %d came %.1f s after the last traffic, want about 50 s",
					pulses+1, gap.Seconds())
			}
			select {
			case next := <-lines:
				if next.text != "BUFFER 0" {
					logs()
					t.Fatalf("after BUFFER 1: %q, want BUFFER 0", next.text)
				}
				if d := next.at.Sub(l.at); d > time.Second {
					t.Fatalf("BUFFER 0 came %v after BUFFER 1", d)
				}
			case <-time.After(5 * time.Second):
				logs()
				t.Fatal("no BUFFER 0 after BUFFER 1")
			}
			previous = l.at
			pulses++
		case <-deadline:
			logs()
			t.Fatalf("%d pulses in 200 s, want 2", pulses)
		}
	}
}
