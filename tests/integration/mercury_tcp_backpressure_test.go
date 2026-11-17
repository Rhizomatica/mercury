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
	"testing"
	"time"
)

// A host that writes more than the ARQ can take at once during a call is
// held back by TCP, not dropped.  The data port's bytes go onto the ARQ's
// payload bus (128 messages) and from there into the 64 kB TX queue; with a
// call being set up and nothing drained yet, a 1 MB write fills both.  Mercury used to read on and
// drop every piece the full bus refused ("Failed to queue ARQ data
// frame(s)"): the host was never told, and in a session the receiver then
// waited for the missing bytes for ever (Go harness, CCIR Poor at 20-24 dB:
// 4 of 18 30 kB sessions stalled with the sender idle).
func TestDataPortWriteBeyondTheQueueIsHeldNotDropped(t *testing.T) {
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
	// A call to a station that is not there: CALLING, a call in progress,
	// for as long as the test runs.
	if got := bw.send(t, "CONNECT TESTA TESTB", "OK", commandTimeout); got == "" {
		fail("CONNECT not answered OK")
	}

	data, err := net.DialTimeout("tcp", fmt.Sprintf("127.0.0.1:%d", basePort+1), 5*time.Second)
	if err != nil {
		fail("data port: %v", err)
	}
	defer data.Close()

	// 1 MB in one write, as a host with a whole file to send does.  (The
	// kernel's loopback buffers may take all of it; what matters is that
	// Mercury takes only what it can queue, and drops nothing.)
	payload := make([]byte, 1<<20)
	for i := range payload {
		payload[i] = byte(i)
	}
	_ = data.SetWriteDeadline(time.Now().Add(4 * time.Second))
	written, _ := data.Write(payload)
	time.Sleep(500 * time.Millisecond)

	logText, err := os.ReadFile(stderr.Name())
	if err != nil {
		fail("read log: %v", err)
	}
	if n := strings.Count(string(logText), "Failed to queue ARQ data"); n > 0 {
		fail("%d piece(s) of the host's write were dropped (%d of %d bytes written)", n, written, len(payload))
	}
	// What Mercury has queued is all it has taken; the rest waits in the
	// sockets.
	got := bw.send(t, "BUFFER", "BUFFER", commandTimeout)
	t.Logf("host wrote %d bytes before the deadline; Mercury reports %q", written, got)
}
