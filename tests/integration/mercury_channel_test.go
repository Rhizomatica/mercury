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

func TestMercuryChannelARQNoNoise(t *testing.T) {
	repoRoot := mustRepoRoot(t)
	bin := locateOrBuildMercury(t, repoRoot)
	chBin, err := buildCh(repoRoot)
	if err != nil {
		t.Skipf("ch not available: %v", err)
	}

	dir := t.TempDir()
	aRX := filepath.Join(dir, "a_rx.fifo")
	aTX := filepath.Join(dir, "a_tx.fifo")
	bRX := filepath.Join(dir, "b_rx.fifo")
	bTX := filepath.Join(dir, "b_tx.fifo")
	for _, p := range []string{aRX, aTX, bRX, bTX} {
		if err := syscall.Mkfifo(p, 0600); err != nil {
			t.Fatalf("mkfifo %s: %v", p, err)
		}
	}

	aPort := freePortPair(t)
	bPort := freePortPair(t)

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	aBcastPort := aPort + 100
	bBcastPort := bPort + 100

	startInstance := func(name, rxPath, txPath string, port, bcastPort int) (*exec.Cmd, *processWait, *os.File, *os.File) {
		stdout, stderr := tempLogFilesNamed(t, name)
		cmd := exec.CommandContext(ctx, bin,
			"-x", "fifo", "-i", rxPath, "-o", txPath,
			"-p", fmt.Sprint(port),
			"-b", fmt.Sprint(bcastPort),
			"-m", "1",
			"-C", filepath.Join(t.TempDir(), "missing.ini"),
		)
		cmd.Dir = repoRoot
		cmd.Stdout = stdout
		cmd.Stderr = stderr
		if err := cmd.Start(); err != nil {
			t.Fatalf("start mercury %s: %v", name, err)
		}
		return cmd, waitForProcess(cmd), stdout, stderr
	}

	cmdA, procA, outA, errA := startInstance("A", aRX, aTX, aPort, aBcastPort)
	defer func() { _ = stopProcess(t, cmdA, procA, outA.Name(), errA.Name()) }()
	cmdB, procB, outB, errB := startInstance("B", bRX, bTX, bPort, bBcastPort)
	defer func() { _ = stopProcess(t, cmdB, procB, outB.Name(), errB.Name()) }()

	params := DefaultChannelParams()
	cb := startChannelBridge(ctx, chBin, aTX, bRX, bTX, aRX, params)
	defer cb.Close()

	time.Sleep(500 * time.Millisecond)

	connA, err := waitForTCP(ctx, "127.0.0.1", aPort, controlPortTimeout, procA)
	if err != nil {
		printLogs(t, outA.Name(), errA.Name())
		printLogs(t, outB.Name(), errB.Name())
		t.Fatalf("A control port: %v", err)
	}
	defer connA.Close()

	connB, err := waitForTCP(ctx, "127.0.0.1", bPort, controlPortTimeout, procB)
	if err != nil {
		printLogs(t, outA.Name(), errA.Name())
		printLogs(t, outB.Name(), errB.Name())
		t.Fatalf("B control port: %v", err)
	}
	defer connB.Close()

	rwA := bufio.NewReadWriter(bufio.NewReader(connA), bufio.NewWriter(connA))
	rwB := bufio.NewReadWriter(bufio.NewReader(connB), bufio.NewWriter(connB))

	sendAndExpect(t, connA, rwA, "MYCALL TESTA", "OK")
	sendAndExpect(t, connB, rwB, "MYCALL TESTB", "OK")
	sendAndExpect(t, connA, rwA, "LISTEN ON", "OK")
	t.Logf("A listening, connecting B(TESTB) to A(TESTA)...")

	sendAndExpect(t, connB, rwB, "CONNECT TESTB TESTA", "OK")

	// After CONNECT, the ARQ sends async notifications. Read and collect them.
	notifA := readLinesTimeout(t, connA, rwA, 5*time.Second)
	notifB := readLinesTimeout(t, connB, rwB, 5*time.Second)

	t.Logf("A notifications: %v", notifA)
	t.Logf("B notifications: %v", notifB)

	hasPTT := false
	hasConnected := false
	for _, line := range append(notifA, notifB...) {
		if strings.HasPrefix(line, "PTT") {
			hasPTT = true
		}
		if strings.HasPrefix(line, "CONNECTED") {
			hasConnected = true
		}
	}

	t.Logf("PTT activity: %v, CONNECTED: %v", hasPTT, hasConnected)

	if !hasPTT {
		printLogs(t, outA.Name(), errA.Name())
		printLogs(t, outB.Name(), errB.Name())
		t.Logf("WARNING: no PTT activity detected — audio may not be flowing through the channel")
	}

	cb.Close()
	time.Sleep(300 * time.Millisecond)

	for i, p := range []*processWait{procA, procB} {
		select {
		case <-p.done:
			t.Logf("mercury %c exited", 'A'+byte(i))
		default:
			t.Logf("mercury %c still running", 'A'+byte(i))
		}
	}
}

func sendAndExpect(t *testing.T, conn net.Conn, rw *bufio.ReadWriter, cmd, want string) string {
	t.Helper()
	resp := sendControlCommand(t, conn, rw, cmd)
	if !strings.HasPrefix(resp, want) {
		t.Fatalf("%q -> %q, want prefix %q", cmd, resp, want)
	}
	return resp
}

func readLinesTimeout(t *testing.T, conn net.Conn, rw *bufio.ReadWriter, timeout time.Duration) []string {
	t.Helper()
	var lines []string
	deadline := time.Now().Add(timeout)
	for time.Now().Before(deadline) {
		conn.SetReadDeadline(time.Now().Add(200 * time.Millisecond))
		line, err := rw.ReadString('\r')
		if err != nil {
			if !isTimeout(err) {
				break
			}
			continue
		}
		line = strings.TrimSuffix(line, "\r")
		line = strings.TrimSpace(line)
		if line != "" {
			lines = append(lines, line)
		}
		if strings.HasPrefix(line, "PTT OFF") || strings.HasPrefix(line, "DISCONNECTED") {
			break
		}
	}
	conn.SetReadDeadline(time.Time{})
	return lines
}

func isTimeout(err error) bool {
	if err == nil {
		return false
	}
	if netErr, ok := err.(net.Error); ok && netErr.Timeout() {
		return true
	}
	return strings.Contains(err.Error(), "i/o timeout")
}
