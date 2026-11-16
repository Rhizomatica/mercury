package integration

import (
	"bufio"
	"context"
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

// A web browser pointed at the control port is told, in HTTP, that the port
// is for host programs, and closed; the data connection already there stays,
// and a host then gets the control port at once (tcp_interfaces.c,
// is_http_request, refuse_web_client).  On air (K7EK, 2026-11-14 03:59)
// Firefox held the control port, each request line and header answered
// WRONG, and its leaving took BPQ32's data connection with it.
func TestBrowserOnControlPortIsTurnedAway(t *testing.T) {
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

	// Wait for the control port, then let that probe go.
	probe, err := waitForTCP(ctx, "127.0.0.1", basePort, controlPortTimeout, proc)
	if err != nil {
		fail("control port: %v", err)
	}
	probe.Close()
	// Its leaving closes any data client; let that pass before ours connects.
	time.Sleep(500 * time.Millisecond)

	// The host's data connection, as BPQ32's was, with no control client.
	data, err := net.DialTimeout("tcp", fmt.Sprintf("127.0.0.1:%d", basePort+1), 2*time.Second)
	if err != nil {
		fail("data port: %v", err)
	}
	defer data.Close()

	// The browser: the request Firefox sent, in one write.
	var web net.Conn
	deadline := time.Now().Add(5 * time.Second)
	for web == nil && time.Now().Before(deadline) {
		web, err = net.DialTimeout("tcp", fmt.Sprintf("127.0.0.1:%d", basePort), time.Second)
		if err != nil {
			web = nil
			time.Sleep(100 * time.Millisecond)
		}
	}
	if web == nil {
		fail("browser could not connect: %v", err)
	}
	defer web.Close()
	req := "GET / HTTP/1.1\r\nHost: 127.0.0.1:" + fmt.Sprint(basePort) +
		"\r\nUser-Agent: Mozilla/5.0\r\nAccept: text/html\r\nConnection: keep-alive\r\n\r\n"
	if _, err := web.Write([]byte(req)); err != nil {
		fail("browser write: %v", err)
	}
	_ = web.SetReadDeadline(time.Now().Add(5 * time.Second))
	reply, err := io.ReadAll(web)
	if err != nil {
		fail("browser was not closed (read: %v; got %q)", err, reply)
	}
	if !strings.HasPrefix(string(reply), "HTTP/1.0 400") || !strings.Contains(string(reply), "TNC control port") {
		fail("browser reply: %q", reply)
	}
	if strings.Contains(string(reply), "WRONG") {
		fail("the request lines were run as TNC commands: %q", reply)
	}

	// The data connection is still there.
	time.Sleep(300 * time.Millisecond)
	if err := stillOpen(data); err != nil {
		fail("data connection after the browser: %v", err)
	}

	// And a host gets the control port at once.
	ctl, err := net.DialTimeout("tcp", fmt.Sprintf("127.0.0.1:%d", basePort), 2*time.Second)
	if err != nil {
		fail("host control connect: %v", err)
	}
	defer ctl.Close()
	rw := bufio.NewReadWriter(bufio.NewReader(ctl), bufio.NewWriter(ctl))
	if got := sendControlCommand(t, ctl, rw, "MYCALL TESTA"); !strings.HasPrefix(got, "OK") {
		fail("host MYCALL after the browser: %q", got)
	}

	logs, _ := os.ReadFile(stdout.Name())
	errs, _ := os.ReadFile(stderr.Name())
	if !strings.Contains(string(logs)+string(errs), "is a web browser") {
		fail("no warning naming the browser in the log")
	}
}
