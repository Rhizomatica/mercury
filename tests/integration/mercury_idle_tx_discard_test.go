package integration

import (
	"bufio"
	"context"
	"fmt"
	"net"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

// Bytes a data client writes with no call up are dropped when it closes
// (#572).  They used to wait in the TX queue and go out as the first payload
// of the next session, with whatever station that was: a readiness probe's
// one newline, written and closed, broke a B2F session that came later.
// Checked idle and then listening, the two states with no call in progress.
func TestIdleDataClientBytesDroppedOnClose(t *testing.T) {
	repoRoot := mustRepoRoot(t)
	bin := locateOrBuildMercury(t, repoRoot)

	basePort := freePortPair(t)
	broadcastPort := freePort(t)
	for broadcastPort == basePort || broadcastPort == basePort+1 {
		broadcastPort = freePort(t)
	}
	stdout, stderr := tempLogFiles(t)
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	cmd := exec.CommandContext(ctx, bin, "-x", "null",
		"-p", fmt.Sprint(basePort), "-b", fmt.Sprint(broadcastPort),
		"-C", filepath.Join(t.TempDir(), "missing-mercury.ini"))
	cmd.Dir = repoRoot
	cmd.Stdout = stdout
	cmd.Stderr = stderr
	if err := startChild(cmd); err != nil {
		t.Fatalf("start mercury: %v", err)
	}
	proc := waitForProcess(cmd)
	defer func() {
		if err := stopProcess(t, cmd, proc, stdout.Name(), stderr.Name()); err != nil && !t.Failed() {
			t.Log(err)
		}
	}()
	fail := func(format string, args ...any) {
		t.Helper()
		printLogs(t, stdout.Name(), stderr.Name())
		t.Fatalf(format, args...)
	}

	ctl, err := waitForTCP(ctx, "127.0.0.1", basePort, controlPortTimeout, proc)
	if err != nil {
		fail("control port: %v", err)
	}
	defer ctl.Close()
	bw := &bufferWatch{conn: ctl, rw: bufio.NewReadWriter(bufio.NewReader(ctl), bufio.NewWriter(ctl))}
	if got := bw.send(t, "MYCALL TESTA", "OK", commandTimeout); got == "" {
		fail("MYCALL not answered OK")
	}
	// No line has said BUFFER yet: the first query answers.
	if got := bw.send(t, "BUFFER", "BUFFER", commandTimeout); got != "BUFFER 0" {
		fail("first BUFFER = %q, want BUFFER 0", got)
	}

	payload := []byte("thirty bytes nobody will read\n") // 30 bytes, as in the report
	for _, phase := range []struct {
		name   string
		before string // control command that sets the state, if any
	}{
		{name: "idle"},
		{name: "listening", before: "LISTEN ON"},
	} {
		if phase.before != "" {
			if got := bw.send(t, phase.before, "OK", commandTimeout); got == "" {
				fail("%s: %q not answered OK", phase.name, phase.before)
			}
		}
		if got, ok := bw.waitFor(t, 0, 3*time.Second); !ok {
			fail("%s: BUFFER before the write = %q, want BUFFER 0", phase.name, got)
		}

		data, err := net.DialTimeout("tcp", fmt.Sprintf("127.0.0.1:%d", basePort+1), 2*time.Second)
		if err != nil {
			fail("%s: data port: %v", phase.name, err)
		}
		if _, err := data.Write(payload); err != nil {
			data.Close()
			fail("%s: data write: %v", phase.name, err)
		}
		// Queued while the writer is there: the host's own bytes are kept
		// as long as it stays (BUFFER is how a host sees its backlog).
		if got, ok := bw.waitFor(t, len(payload), 3*time.Second); !ok {
			data.Close()
			fail("%s: BUFFER after writing %d bytes = %q, want BUFFER %d",
				phase.name, len(payload), got, len(payload))
		}

		data.Close()
		if got, ok := bw.waitFor(t, 0, 3*time.Second); !ok {
			fail("%s: BUFFER after the data client closed = %q, want BUFFER 0 "+
				"(the bytes would go out on the next session)", phase.name, got)
		}
		t.Logf("%s: %d bytes queued, dropped when the data client closed", phase.name, len(payload))
	}

	log := readLog(t, stdout.Name(), stderr.Name())
	if n := strings.Count(log, "Data client left with no session up"); n != 2 {
		fail("discard logged %d times, want 2 (once per phase)", n)
	}
}

// bufferWatch reads the control connection and keeps the last BUFFER value
// Mercury sent there, asked for or not.  Mercury sends a BUFFER line only
// when the value changes (tnc_send_buffer), a query included, so the last
// one seen is the backlog as the host knows it.
type bufferWatch struct {
	conn net.Conn
	rw   *bufio.ReadWriter
	last string
}

// send writes a control command and reads lines for up to wait, keeping the
// last BUFFER line.  It returns the first line that starts with want, or ""
// if none came.
func (b *bufferWatch) send(t *testing.T, command, want string, wait time.Duration) string {
	t.Helper()
	if _, err := b.rw.WriteString(command + "\r"); err != nil {
		t.Fatalf("write %q: %v", command, err)
	}
	if err := b.rw.Flush(); err != nil {
		t.Fatalf("flush %q: %v", command, err)
	}
	found := ""
	deadline := time.Now().Add(wait)
	for time.Now().Before(deadline) {
		if err := b.conn.SetReadDeadline(deadline); err != nil {
			t.Fatal(err)
		}
		line, err := b.rw.ReadString('\r')
		if err != nil {
			break // read deadline: nothing more for now
		}
		line = strings.TrimSuffix(line, "\r")
		if strings.HasPrefix(line, "BUFFER") {
			b.last = line
		}
		if found == "" && want != "" && strings.HasPrefix(line, want) {
			found = line
			if want != "BUFFER" {
				break
			}
		}
	}
	return found
}

// waitFor asks BUFFER until the last BUFFER line reads want or timeout
// passes, and returns that last line.
func (b *bufferWatch) waitFor(t *testing.T, want int, timeout time.Duration) (string, bool) {
	t.Helper()
	wantLine := fmt.Sprintf("BUFFER %d", want)
	deadline := time.Now().Add(timeout)
	for time.Now().Before(deadline) {
		b.send(t, "BUFFER", "", 200*time.Millisecond)
		if b.last == wantLine {
			return b.last, true
		}
	}
	return b.last, false
}

// An idle write larger than the TX queue (64 KiB): the payload bridge
// blocks on the full queue with nothing draining it, and a single clear at
// the close woke it to write the rest after the clear, still bound for the
// next session (review of #580).  Every byte must be gone, and stay gone.
func TestIdleDataClientOversizeWriteDroppedOnClose(t *testing.T) {
	repoRoot := mustRepoRoot(t)
	bin := locateOrBuildMercury(t, repoRoot)

	basePort := freePortPair(t)
	broadcastPort := freePort(t)
	for broadcastPort == basePort || broadcastPort == basePort+1 {
		broadcastPort = freePort(t)
	}
	stdout, stderr := tempLogFiles(t)
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	cmd := exec.CommandContext(ctx, bin, "-x", "null",
		"-p", fmt.Sprint(basePort), "-b", fmt.Sprint(broadcastPort),
		"-C", filepath.Join(t.TempDir(), "missing-mercury.ini"))
	cmd.Dir = repoRoot
	cmd.Stdout = stdout
	cmd.Stderr = stderr
	if err := startChild(cmd); err != nil {
		t.Fatalf("start mercury: %v", err)
	}
	proc := waitForProcess(cmd)
	defer func() {
		if err := stopProcess(t, cmd, proc, stdout.Name(), stderr.Name()); err != nil && !t.Failed() {
			t.Log(err)
		}
	}()
	fail := func(format string, args ...any) {
		t.Helper()
		printLogs(t, stdout.Name(), stderr.Name())
		t.Fatalf(format, args...)
	}

	ctl, err := waitForTCP(ctx, "127.0.0.1", basePort, controlPortTimeout, proc)
	if err != nil {
		fail("control port: %v", err)
	}
	defer ctl.Close()
	bw := &bufferWatch{conn: ctl, rw: bufio.NewReadWriter(bufio.NewReader(ctl), bufio.NewWriter(ctl))}
	if got := bw.send(t, "MYCALL TESTA", "OK", commandTimeout); got == "" {
		fail("MYCALL not answered OK")
	}
	if got := bw.send(t, "LISTEN ON", "OK", commandTimeout); got == "" {
		fail("LISTEN ON not answered OK")
	}

	data, err := net.DialTimeout("tcp", fmt.Sprintf("127.0.0.1:%d", basePort+1), 2*time.Second)
	if err != nil {
		fail("data port: %v", err)
	}
	// Three times the queue, in the bridge's 4 KiB messages.  Mercury may
	// stop reading once the queue is full, so the write runs in the
	// background and the close cuts it short.
	big := make([]byte, 192*1024)
	for i := range big {
		big[i] = byte('a' + i%26)
	}
	go func() { _, _ = data.Write(big) }()
	if got, ok := bw.waitFor(t, 64*1024, 5*time.Second); !ok {
		data.Close()
		fail("BUFFER while writing %d bytes = %q, want the full queue (BUFFER 65536)", len(big), got)
	}
	time.Sleep(300 * time.Millisecond) // let the bridge block on the full queue

	data.Close()
	if got, ok := bw.waitFor(t, 0, 3*time.Second); !ok {
		fail("BUFFER after the data client closed = %q, want BUFFER 0", got)
	}
	// And it stays empty: nothing the bridge held lands after the clear.
	time.Sleep(1500 * time.Millisecond)
	if got := bw.send(t, "BUFFER", "BUFFER", 500*time.Millisecond); got != "" && got != "BUFFER 0" {
		fail("BUFFER 1.5 s after the close = %q, want BUFFER 0 (bytes landed after the clear)", got)
	}
	if bw.last != "BUFFER 0" {
		fail("last BUFFER line 1.5 s after the close = %q, want BUFFER 0", bw.last)
	}
	t.Log("full queue plus a blocked bridge: every byte dropped when the data client closed")
}
