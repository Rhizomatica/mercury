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

// A web page can send a request to any port on this machine, with a body and
// a URL of its choosing.  Nothing in such a request may be run as a TNC
// command or transmitted as a broadcast frame, whatever the length of its
// request line and however the page words it (http or https).

type webGuardMercury struct {
	control   string
	broadcast string
	logs      func() string
	fail      func(format string, args ...any)
}

func startWebGuardMercury(t *testing.T) *webGuardMercury {
	t.Helper()
	repoRoot := mustRepoRoot(t)
	bin := locateOrBuildMercury(t, repoRoot)

	basePort := freePortPair(t)
	broadcastPort := freePort(t)
	for broadcastPort == basePort || broadcastPort == basePort+1 {
		broadcastPort = freePort(t)
	}
	stdout, stderr := tempLogFiles(t)
	ctx, cancel := context.WithCancel(context.Background())
	t.Cleanup(cancel)

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
	t.Cleanup(func() {
		if err := stopProcess(t, cmd, proc, stdout.Name(), stderr.Name()); err != nil && !t.Failed() {
			t.Log(err)
		}
	})

	m := &webGuardMercury{
		control:   fmt.Sprintf("127.0.0.1:%d", basePort),
		broadcast: fmt.Sprintf("127.0.0.1:%d", broadcastPort),
	}
	m.logs = func() string {
		o, _ := os.ReadFile(stdout.Name())
		e, _ := os.ReadFile(stderr.Name())
		return string(o) + string(e)
	}
	m.fail = func(format string, args ...any) {
		t.Helper()
		printLogs(t, stdout.Name(), stderr.Name())
		t.Fatalf(format, args...)
	}
	probe, err := waitForTCP(ctx, "127.0.0.1", basePort, controlPortTimeout, proc)
	if err != nil {
		m.fail("control port: %v", err)
	}
	probe.Close()
	time.Sleep(500 * time.Millisecond)
	return m
}

// dial retries: the port takes one client at a time.
func (m *webGuardMercury) dial(t *testing.T, addr string) net.Conn {
	t.Helper()
	deadline := time.Now().Add(5 * time.Second)
	for {
		c, err := net.DialTimeout("tcp", addr, time.Second)
		if err == nil {
			t.Cleanup(func() { c.Close() })
			return c
		}
		if time.Now().After(deadline) {
			m.fail("dial %s: %v", addr, err)
		}
		time.Sleep(100 * time.Millisecond)
	}
}

// browserPost is a cross-site fetch() with a text/plain body, which a browser
// sends without asking first.  The page picks the path and the body.
func browserPost(path string, body []byte) []byte {
	head := fmt.Sprintf("POST %s HTTP/1.1\r\nHost: 127.0.0.1\r\nOrigin: https://example.org\r\n"+
		"User-Agent: Mozilla/5.0\r\nAccept: */*\r\nContent-Type: text/plain\r\nContent-Length: %d\r\n\r\n",
		path, len(body))
	return append([]byte(head), body...)
}

// closedByPeer reads until Mercury ends the connection; a reset counts.
func closedByPeer(c net.Conn) (string, error) {
	_ = c.SetReadDeadline(time.Now().Add(5 * time.Second))
	got, err := io.ReadAll(c)
	if ne, ok := err.(net.Error); ok && ne.Timeout() {
		return string(got), fmt.Errorf("still open after 5 s")
	}
	return string(got), nil
}

func TestWebRequestWithALongURLRunsNoControlCommand(t *testing.T) {
	// The control port reads 128 bytes at a time: from a path of about 110
	// characters on, " HTTP/" is not in the first read.
	for _, pathLen := range []int{0, 150, 2000, 16000} {
		t.Run(fmt.Sprintf("path_%d", pathLen), func(t *testing.T) {
			m := startWebGuardMercury(t)
			web := m.dial(t, m.control)
			req := browserPost("/"+strings.Repeat("a", pathLen), []byte("\rMYCALL TESTWEB\r"))
			if _, err := web.Write(req); err != nil {
				m.fail("browser write: %v", err)
			}
			reply, err := closedByPeer(web)
			if err != nil {
				m.fail("browser was not turned away: %v; got %.200q", err, reply)
			}
			time.Sleep(300 * time.Millisecond)
			if strings.Contains(reply, "OK\r") || strings.Contains(reply, "WRONG") ||
				strings.Contains(m.logs(), "TESTWEB") {
				m.fail("the request was run as TNC commands: %.300q", reply)
			}

			// A host still gets the port and is served.
			ctl := m.dial(t, m.control)
			rw := bufio.NewReadWriter(bufio.NewReader(ctl), bufio.NewWriter(ctl))
			if got := sendControlCommand(t, ctl, rw, "MYCALL TESTA"); !strings.HasPrefix(got, "OK") {
				m.fail("host MYCALL after the browser: %q", got)
			}
		})
	}
}

// kissFrame is a data frame (command 0) as a KISS host sends it.
func kissFrame(payload string) []byte {
	return append(append([]byte{0xC0, 0x00}, payload...), 0xC0)
}

// tlsHello is the start of an https:// request: a TLS record, whose real
// cipher list holds 0xC0 bytes (KISS's frame delimiter) such as C0 2B.
func tlsHello() []byte {
	hello := []byte{0x16, 0x03, 0x01, 0x00, 0x40, 0x01, 0x00, 0x00, 0x3c, 0x03, 0x03}
	return append(hello, kissFrame("TLS-WEB-BYTES")...)
}

func TestWebRequestOnBroadcastPortIsNotTransmitted(t *testing.T) {
	cases := map[string][]byte{
		"post":     browserPost("/", append([]byte("\r\n"), kissFrame("WEB-FRAME")...)),
		"post_long": browserPost("/"+strings.Repeat("a", 4000), kissFrame("WEB-FRAME")),
		"get":      append([]byte("GET /"), append(kissFrame("WEB-FRAME"), " HTTP/1.1\r\n\r\n"...)...),
		"https":    tlsHello(),
	}
	for name, req := range cases {
		t.Run(name, func(t *testing.T) {
			m := startWebGuardMercury(t)
			web := m.dial(t, m.broadcast)
			if _, err := web.Write(req); err != nil {
				m.fail("browser write: %v", err)
			}
			if reply, err := closedByPeer(web); err != nil {
				m.fail("browser was not turned away: %v; got %.200q", err, reply)
			}
			time.Sleep(300 * time.Millisecond)
			if strings.Contains(m.logs(), "KISS frame decoded") {
				m.fail("bytes of a web request were read as a broadcast frame")
			}
		})
	}
}

// A second client of the broadcast port waits in the backlog (#588), so the
// check must also hold for a web request read after its sender has gone.
func TestWebRequestQueuedOnBroadcastPortIsNotTransmitted(t *testing.T) {
	m := startWebGuardMercury(t)
	holder := m.dial(t, m.broadcast)
	time.Sleep(500 * time.Millisecond)

	web := m.dial(t, m.broadcast)
	if _, err := web.Write(browserPost("/", kissFrame("QUEUED-WEB-FRAME"))); err != nil {
		m.fail("browser write: %v", err)
	}
	web.Close()
	holder.Close()
	time.Sleep(2 * time.Second)

	if strings.Contains(m.logs(), "KISS frame decoded") {
		m.fail("a queued web request was read as a broadcast frame")
	}
}

// An ordinary KISS host is not affected, before or after a web client.
func TestKissHostStillServedAfterAWebRequest(t *testing.T) {
	m := startWebGuardMercury(t)
	web := m.dial(t, m.broadcast)
	if _, err := web.Write(browserPost("/", kissFrame("WEB-FRAME"))); err != nil {
		m.fail("browser write: %v", err)
	}
	_, _ = closedByPeer(web)

	host := m.dial(t, m.broadcast)
	if _, err := host.Write(kissFrame("HOST-FRAME")); err != nil {
		m.fail("host write: %v", err)
	}
	deadline := time.Now().Add(5 * time.Second)
	for !strings.Contains(m.logs(), "KISS frame decoded: cmd=0x00 len=10") {
		if time.Now().After(deadline) {
			m.fail("the KISS host's frame was not read")
		}
		time.Sleep(100 * time.Millisecond)
	}
}
