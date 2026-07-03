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
	"syscall"
	"testing"
	"time"
)

// TestMercuryARQLargeQueueNoWedge is the end-to-end regression guard for the
// app-TX-ring deadlock: queueing more data than APP_TX_BUF_SIZE (64 KB) used
// to park arq_payload_bridge_worker inside write_buffer() while it held
// g_app_tx_mtx, which starved cb_tx_read and wedged the whole ARQ event loop
// (no data frames, no keepalives, permanently).
//
// The test connects two instances over a clean channel, pushes 80 KB into the
// caller's data port in one burst, and requires that ANY payload reaches the
// peer.  It deliberately does not wait for the full transfer (that would take
// tens of minutes at HF rates): the wedge fires the instant the ring fills —
// milliseconds after the burst, long before the first data frame completes —
// so the first delivered byte already proves the event loop survived it.
func TestMercuryARQLargeQueueNoWedge(t *testing.T) {
	repoRoot := mustRepoRoot(t)
	bin := locateOrBuildMercury(t, repoRoot)

	params := DefaultChannelParams()
	chBin, err := buildCh(repoRoot)
	if err != nil {
		t.Skipf("channel simulator unavailable: %v", err)
	}

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

	aPort := freePortPair(t)
	bPort := freePortPair(t)

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	startInstance := func(name, rxPath, txPath string, port, bcastPort int) (*exec.Cmd, *processWait, *os.File, *os.File) {
		stdout, stderr := tempLogFilesNamed(t, name)
		cmd := exec.CommandContext(ctx, bin,
			"-x", "fifo",
			"-i", rxPath,
			"-o", txPath,
			"-p", fmt.Sprint(port),
			"-b", fmt.Sprint(bcastPort),
			"-m", "1",
			"-C", filepath.Join(t.TempDir(), "missing-mercury.ini"),
		)
		cmd.Dir = repoRoot
		cmd.Stdout = stdout
		cmd.Stderr = stderr
		if err := cmd.Start(); err != nil {
			t.Fatalf("start mercury %s: %v", name, err)
		}
		return cmd, waitForProcess(cmd), stdout, stderr
	}

	cmdA, procA, outA, errA := startInstance("A", aRX, aTX, aPort, aPort+100)
	defer func() { _ = stopProcess(t, cmdA, procA, outA.Name(), errA.Name()) }()
	cmdB, procB, outB, errB := startInstance("B", bRX, bTX, bPort, bPort+100)
	defer func() { _ = stopProcess(t, cmdB, procB, outB.Name(), errB.Name()) }()

	bridge := startChannelBridge(ctx, chBin, aTX, bRX, bTX, aRX, params)
	defer bridge.Close()

	failWithLogs := func(format string, args ...interface{}) {
		printLogs(t, outA.Name(), errA.Name())
		printLogs(t, outB.Name(), errB.Name())
		t.Fatalf(format, args...)
	}

	connA, err := waitForTCP(ctx, "127.0.0.1", aPort, controlPortTimeout, procA)
	if err != nil {
		failWithLogs("mercury A control port: %v", err)
	}
	defer connA.Close()
	connB, err := waitForTCP(ctx, "127.0.0.1", bPort, controlPortTimeout, procB)
	if err != nil {
		failWithLogs("mercury B control port: %v", err)
	}
	defer connB.Close()

	rwA := bufio.NewReadWriter(bufio.NewReader(connA), bufio.NewWriter(connA))
	rwB := bufio.NewReadWriter(bufio.NewReader(connB), bufio.NewWriter(connB))

	for _, c := range []struct {
		conn net.Conn
		rw   *bufio.ReadWriter
		cmd  string
	}{
		{connA, rwA, "MYCALL TESTA"},
		{connB, rwB, "MYCALL TESTB"},
		{connB, rwB, "LISTEN ON"},
	} {
		if got := sendControlCommand(t, c.conn, c.rw, c.cmd); !strings.HasPrefix(got, "OK") {
			failWithLogs("%q -> %q, want OK", c.cmd, got)
		}
	}

	dataA, err := net.DialTimeout("tcp", fmt.Sprintf("127.0.0.1:%d", aPort+1), 5*time.Second)
	if err != nil {
		failWithLogs("mercury A data port: %v", err)
	}
	defer dataA.Close()
	dataB, err := net.DialTimeout("tcp", fmt.Sprintf("127.0.0.1:%d", bPort+1), 5*time.Second)
	if err != nil {
		failWithLogs("mercury B data port: %v", err)
	}
	defer dataB.Close()

	if got := sendControlCommand(t, connA, rwA, "CONNECT TESTA TESTB"); !strings.HasPrefix(got, "OK") {
		failWithLogs("CONNECT -> %q, want OK", got)
	}

	connectedDeadline := time.Now().Add(3 * time.Minute)
	if err := connA.SetReadDeadline(connectedDeadline); err != nil {
		t.Fatal(err)
	}
	connected := false
	for time.Now().Before(connectedDeadline) && !connected {
		line, err := rwA.ReadString('\r')
		if err != nil {
			break
		}
		line = strings.TrimSpace(line)
		if strings.HasPrefix(line, "CONNECTED") {
			connected = true
		}
		if strings.HasPrefix(line, "DISCONNECTED") {
			failWithLogs("link disconnected before CONNECT completed")
		}
	}
	if !connected {
		failWithLogs("no CONNECTED notification within deadline")
	}
	_ = connA.SetReadDeadline(time.Time{})

	// One 80 KB burst: comfortably beyond the 64 KB app TX ring, so the
	// bridge worker is guaranteed to hit a full ring and park in
	// write_buffer().  The socket write itself may stall once Mercury's
	// reader stops draining (which is itself a wedge symptom on broken
	// builds), so write with a deadline and only require that enough got
	// through to overflow the ring.
	const ringSize = 64 * 1024
	payload := []byte(strings.Repeat("MERCURY-LARGE-QUEUE-0123456789ABCD", (80*1024)/34+1))
	if err := dataA.SetWriteDeadline(time.Now().Add(30 * time.Second)); err != nil {
		t.Fatal(err)
	}
	written, werr := dataA.Write(payload)
	t.Logf("queued %d bytes (err=%v)", written, werr)
	if written <= ringSize {
		failWithLogs("could not queue past the %d-byte ring (wrote %d): %v",
			ringSize, written, werr)
	}

	// The wedge (if present) has fired by now.  Any delivered byte proves
	// the event loop is still processing: frames are being read from the
	// ring, modulated, decoded and handed to B's data port.
	rxDeadline := time.Now().Add(3 * time.Minute)
	buf := make([]byte, 4096)
	got := 0
	for got == 0 && time.Now().Before(rxDeadline) {
		if err := dataB.SetReadDeadline(time.Now().Add(10 * time.Second)); err != nil {
			t.Fatal(err)
		}
		n, err := dataB.Read(buf)
		if n > 0 {
			got += n
		}
		if err != nil {
			if ne, ok := err.(net.Error); ok && ne.Timeout() {
				continue
			}
			break
		}
	}
	if got == 0 {
		failWithLogs("no payload delivered after queueing %d bytes: ARQ event loop wedged (TX-ring deadlock regression)", written)
	}
	t.Logf("event loop alive after %d-byte queue: %d bytes already delivered", written, got)
}
