//go:build linux

package integration

import (
	"bufio"
	"context"
	"fmt"
	"math/rand"
	"net"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

type processWaitHandle struct {
	cmd      *exec.Cmd
	proc     *processWait
	out, err string
}

// TestMercuryARQTransferRTP runs an ARQ session between two Mercury
// instances on the -x rtp backend, with rtpRadioPair playing both radio
// daemons: the radios own the sample clock, TX is paced by it, and PTT
// travels in the TX stream.  The transfer must complete and every
// transmission must be keyed by a marker packet and ended by an empty
// packet (none by the dead-keyer), with TX in lockstep with RX.
func TestMercuryARQTransferRTP(t *testing.T) {
	repoRoot := mustRepoRoot(t)
	bin := locateOrBuildMercury(t, repoRoot)

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	base := byte(100 + rand.Intn(100))
	radios, err := startRTPRadioPair(ctx, base)
	if err != nil {
		t.Skipf("loopback multicast unavailable: %v", err)
	}
	defer radios.Close()

	aPort := freePortPair(t)
	bPort := freePortPair(t)
	start := func(name string, idx, port int) (*processWaitHandle, error) {
		rx, tx := rtpGroups(base, idx)
		stdout, stderr := tempLogFilesNamed(t, name)
		cmd := exec.CommandContext(ctx, bin,
			"-x", "rtp", "-i", rx.String()+",lo", "-o", tx.String(),
			"-p", fmt.Sprint(port), "-b", fmt.Sprint(port+100), "-m", "1",
			"-C", filepath.Join(t.TempDir(), "missing-mercury.ini"))
		cmd.Dir = repoRoot
		cmd.Stdout = stdout
		cmd.Stderr = stderr
		if err := startChild(cmd); err != nil {
			return nil, err
		}
		return &processWaitHandle{cmd: cmd, proc: waitForProcess(cmd), out: stdout.Name(), err: stderr.Name()}, nil
	}
	a, err := start("A", 0, aPort)
	if err != nil {
		t.Fatalf("start A: %v", err)
	}
	defer func() { _ = stopProcess(t, a.cmd, a.proc, a.out, a.err) }()
	b, err := start("B", 1, bPort)
	if err != nil {
		t.Fatalf("start B: %v", err)
	}
	defer func() { _ = stopProcess(t, b.cmd, b.proc, b.out, b.err) }()

	fail := func(format string, args ...interface{}) {
		printLogs(t, a.out, a.err)
		printLogs(t, b.out, b.err)
		t.Log(radios.Stats())
		t.Fatalf(format, args...)
	}

	connA, err := waitForTCP(ctx, "127.0.0.1", aPort, controlPortTimeout, a.proc)
	if err != nil {
		fail("mercury A control port: %v", err)
	}
	defer connA.Close()
	connB, err := waitForTCP(ctx, "127.0.0.1", bPort, controlPortTimeout, b.proc)
	if err != nil {
		fail("mercury B control port: %v", err)
	}
	defer connB.Close()
	rwA := bufio.NewReadWriter(bufio.NewReader(connA), bufio.NewWriter(connA))
	rwB := bufio.NewReadWriter(bufio.NewReader(connB), bufio.NewWriter(connB))

	for _, c := range []struct {
		conn  net.Conn
		rw    *bufio.ReadWriter
		cmd   string
		drain string
	}{
		{connA, rwA, "MYCALL TESTA", "REGISTERED TESTA"},
		{connB, rwB, "MYCALL TESTB", "REGISTERED TESTB"},
		{connB, rwB, "LISTEN ON", ""},
	} {
		if got := sendControlCommand(t, c.conn, c.rw, c.cmd); !strings.HasPrefix(got, "OK") {
			fail("%q -> %q, want OK", c.cmd, got)
		}
		if c.drain != "" {
			if line, err := c.rw.ReadString('\r'); err != nil || !strings.HasPrefix(line, c.drain) {
				fail("after %q: got %q (%v), want %q", c.cmd, line, err, c.drain)
			}
		}
	}

	dataA, err := net.DialTimeout("tcp", fmt.Sprintf("127.0.0.1:%d", aPort+1), 5*time.Second)
	if err != nil {
		fail("mercury A data port: %v", err)
	}
	defer dataA.Close()
	dataB, err := net.DialTimeout("tcp", fmt.Sprintf("127.0.0.1:%d", bPort+1), 5*time.Second)
	if err != nil {
		fail("mercury B data port: %v", err)
	}
	defer dataB.Close()

	if got := sendControlCommand(t, connA, rwA, "CONNECT TESTA TESTB"); !strings.HasPrefix(got, "OK") {
		fail("CONNECT -> %q, want OK", got)
	}
	deadline := time.Now().Add(3 * time.Minute)
	_ = connA.SetReadDeadline(deadline)
	connected := false
	for !connected && time.Now().Before(deadline) {
		line, err := rwA.ReadString('\r')
		if err != nil {
			break
		}
		line = strings.TrimSpace(line)
		if line != "" {
			t.Logf("A control: %s", line)
		}
		if strings.HasPrefix(line, "DISCONNECTED") {
			fail("link disconnected before CONNECT completed")
		}
		connected = strings.HasPrefix(line, "CONNECTED")
	}
	if !connected {
		fail("no CONNECTED notification within deadline")
	}
	_ = connA.SetReadDeadline(time.Time{})

	payload := []byte(strings.Repeat("MERCURY-RTP-PAYLOAD-0123456789ABCD", 3))
	if _, err := dataA.Write(payload); err != nil {
		fail("write payload: %v", err)
	}
	rx := make([]byte, 0, len(payload))
	buf := make([]byte, 4096)
	rxDeadline := time.Now().Add(5 * time.Minute)
	for len(rx) < len(payload) && time.Now().Before(rxDeadline) {
		_ = dataB.SetReadDeadline(time.Now().Add(15 * time.Second))
		n, err := dataB.Read(buf)
		rx = append(rx, buf[:n]...)
		if err != nil {
			if ne, ok := err.(net.Error); ok && ne.Timeout() {
				continue
			}
			break
		}
	}
	if string(rx) != string(payload) {
		fail("payload mismatch: got %d bytes, want %d", len(rx), len(payload))
	}

	// Let the last transmission end before checking the PTT accounting.
	time.Sleep(3 * time.Second)
	t.Log(radios.Stats())
	if err := radios.Check(); err != nil {
		fail("PTT contract: %v", err)
	}
	t.Logf("ARQ transfer over RTP complete: %d bytes", len(payload))
}
