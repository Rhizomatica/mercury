package integration

import (
	"bufio"
	"context"
	"fmt"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

// While a call is being placed, the host hears a BITRATE line at least every
// 20 s (arq.c, call_progress).  BPQ32 closes a session idle for its IDLETIME,
// which a forwarding connect script may set to 60 s, and while its port is
// attached and not connected the only Mercury lines that reach the session
// are those its VARA driver does not take itself, BITRATE among them.  On air
// (KG9NI, 2026-11-10/11) 8 calls were dropped 61-62 s after the last line,
// during the fast CALLs, before the first MFSK CALL's BITRATE.
//
// A CALL to a station that is not there, on the null sound system: the CALLs
// go out on their real timing and nothing answers.
func TestCallingKeepsTheHostLineAlive(t *testing.T) {
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

	conn, err := waitForTCP(ctx, "127.0.0.1", basePort, controlPortTimeout, proc)
	if err != nil {
		printLogs(t, stdout.Name(), stderr.Name())
		t.Fatalf("control port: %v", err)
	}
	defer conn.Close()
	rw := bufio.NewReadWriter(bufio.NewReader(conn), bufio.NewWriter(conn))

	for _, c := range []string{"MYCALL N0CALL", "LISTEN OFF"} {
		if got := sendControlCommand(t, conn, rw, c); !strings.HasPrefix(got, "OK") {
			t.Fatalf("%q: got %q", c, got)
		}
		if strings.HasPrefix(c, "MYCALL") {
			if line, err := rw.ReadString('\r'); err != nil || !strings.HasPrefix(line, "REGISTERED") {
				t.Fatalf("after MYCALL: %q, %v", line, err)
			}
		}
	}
	if _, err := rw.WriteString("CONNECT N0CALL N0BODY\r"); err != nil {
		t.Fatal(err)
	}
	if err := rw.Flush(); err != nil {
		t.Fatal(err)
	}

	const watch = 70 * time.Second
	const maxGap = 25 * time.Second
	start := time.Now()
	last := start
	var worst time.Duration
	var lines []string
	n := 0
	for time.Since(start) < watch {
		if err := conn.SetReadDeadline(time.Now().Add(watch - time.Since(start))); err != nil {
			t.Fatal(err)
		}
		line, err := rw.ReadString('\r')
		if err != nil {
			break // the watch is over
		}
		line = strings.TrimSuffix(line, "\r")
		now := time.Now()
		lines = append(lines, fmt.Sprintf("%5.1f %s", now.Sub(start).Seconds(), line))
		if !strings.HasPrefix(line, "BITRATE") {
			continue
		}
		if gap := now.Sub(last); gap > worst {
			worst = gap
		}
		last = now
		n++
	}
	if gap := time.Since(start) - last.Sub(start); gap > worst && n > 0 {
		worst = gap
	}
	if n == 0 || worst > maxGap {
		printLogs(t, stdout.Name(), stderr.Name())
		t.Fatalf("%d BITRATE lines in %v, longest gap %v (want <= %v):\n%s",
			n, watch, worst.Round(100*time.Millisecond), maxGap, strings.Join(lines, "\n"))
	}
	t.Logf("%d BITRATE lines in %v, longest gap %v", n, watch, worst.Round(100*time.Millisecond))
}
