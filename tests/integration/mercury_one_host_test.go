package integration

import (
	"bufio"
	"context"
	"errors"
	"fmt"
	"io"
	"net"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

// A second program that connects to the control or data port while a host
// is connected is refused, and the host keeps the TNC (tcp_interfaces.c,
// client_gone, refuse_client).  It used to take over: on air (KG9NI,
// 2026-11-12, 16:26-18:11) two BPQ32 ports set up for one Mercury took it
// from each other every 3 to 7 s for nearly two hours.  Once the host leaves,
// the next connection gets in, as a host that restarts does.
func TestSecondHostIsRefused(t *testing.T) {
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

	// Host A: control and data.
	ctlA, err := waitForTCP(ctx, "127.0.0.1", basePort, controlPortTimeout, proc)
	if err != nil {
		fail("control port: %v", err)
	}
	defer ctlA.Close()
	rwA := bufio.NewReadWriter(bufio.NewReader(ctlA), bufio.NewWriter(ctlA))
	dataA, err := net.DialTimeout("tcp", fmt.Sprintf("127.0.0.1:%d", basePort+1), 2*time.Second)
	if err != nil {
		fail("data port: %v", err)
	}
	defer dataA.Close()
	if got := sendControlCommand(t, ctlA, rwA, "MYCALL TESTA"); !strings.HasPrefix(got, "OK") {
		fail("A MYCALL: %q", got)
	}
	if line, err := rwA.ReadString('\r'); err != nil || !strings.HasPrefix(line, "REGISTERED") {
		fail("A after MYCALL: %q, %v", line, err)
	}

	// Host B knocks on the control port, again and again as BPQ32 does,
	// and once on the data port: each is closed, A keeps both.
	for i := 0; i < 5; i++ {
		if !closedByMercury(t, basePort) {
			fail("B's control connection %d was not closed: it was let in beside A", i+1)
		}
		time.Sleep(200 * time.Millisecond)
	}
	if !closedByMercury(t, basePort+1) {
		fail("B's data connection was not closed: it was let in beside A")
	}
	if got := sendControlCommand(t, ctlA, rwA, "BUFFER"); !strings.HasPrefix(got, "BUFFER") {
		fail("A after B's knocks: BUFFER -> %q (A lost the control port)", got)
	}
	if err := stillOpen(dataA); err != nil {
		fail("A's data connection after B's knocks: %v", err)
	}

	// A leaves; B gets in.
	ctlA.Close()
	dataA.Close()
	var ctlB net.Conn
	deadline := time.Now().Add(5 * time.Second)
	for time.Now().Before(deadline) {
		c, err := net.DialTimeout("tcp", fmt.Sprintf("127.0.0.1:%d", basePort), time.Second)
		if err == nil {
			rwB := bufio.NewReadWriter(bufio.NewReader(c), bufio.NewWriter(c))
			if err := c.SetDeadline(time.Now().Add(commandTimeout)); err == nil {
				if _, err := rwB.WriteString("MYCALL TESTB\r"); err == nil && rwB.Flush() == nil {
					if line, err := rwB.ReadString('\r'); err == nil && strings.HasPrefix(line, "OK") {
						ctlB = c
						break
					}
				}
			}
			c.Close()
		}
		time.Sleep(100 * time.Millisecond)
	}
	if ctlB == nil {
		fail("B could not take the control port after A left")
	}
	defer ctlB.Close()

	// Said once per port, not once per knock.
	log := readLog(t, stdout.Name(), stderr.Name())
	if n := strings.Count(log, "control connection from"); n != 1 {
		fail("control refusals explained %d times, want 1", n)
	}
	if n := strings.Count(log, "data connection from"); n != 1 {
		fail("data refusals explained %d times, want 1", n)
	}
	for _, l := range strings.Split(log, "\n") {
		if strings.Contains(l, "refused") || strings.Contains(l, "Control client") {
			t.Log(l)
		}
	}
	t.Log("B refused 5 times on control and once on data while A was connected; let in after A left")
}

// closedByMercury connects to port and reports whether Mercury closes the
// connection within 2 s without a word.
func closedByMercury(t *testing.T, port int) bool {
	t.Helper()
	c, err := net.DialTimeout("tcp", fmt.Sprintf("127.0.0.1:%d", port), time.Second)
	if err != nil {
		return true // refused outright counts too
	}
	defer c.Close()
	_ = c.SetReadDeadline(time.Now().Add(2 * time.Second))
	buf := make([]byte, 64)
	for {
		_, err := c.Read(buf)
		if err == nil {
			continue // a line meant for a client: keep reading
		}
		var ne net.Error
		if errors.As(err, &ne) && ne.Timeout() {
			return false // still open after 2 s: let in
		}
		return true // EOF or reset: closed by Mercury
	}
}

// stillOpen reports an error if Mercury closed conn.
func stillOpen(conn net.Conn) error {
	_ = conn.SetReadDeadline(time.Now().Add(300 * time.Millisecond))
	buf := make([]byte, 1)
	_, err := conn.Read(buf)
	var ne net.Error
	if err == nil || (errors.As(err, &ne) && ne.Timeout()) {
		return nil
	}
	if errors.Is(err, io.EOF) {
		return errors.New("closed by Mercury")
	}
	return err
}

func readLog(t *testing.T, paths ...string) string {
	t.Helper()
	var b strings.Builder
	for _, p := range paths {
		data, err := os.ReadFile(p)
		if err == nil {
			b.Write(data)
		}
	}
	return b.String()
}
