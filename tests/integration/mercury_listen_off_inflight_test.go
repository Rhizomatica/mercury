package integration

import (
	"bufio"
	"context"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"syscall"
	"testing"
	"time"
)

// A scanner's LISTEN OFF that cuts a CALL off says how far into the frame it
// came.  Every such line used to read 0.0 s: the RX loop passes more often
// than a decoder reports, and took a pass with no report for a lost lock.
func TestListenOffSaysHowFarIntoTheFrame(t *testing.T) {
	repoRoot := mustRepoRoot(t)
	bin := locateOrBuildMercury(t, repoRoot)
	dir := t.TempDir()
	p := func(s string) string { return filepath.Join(dir, s) }
	for _, f := range []string{"ar", "at", "br", "bt"} {
		if err := syscall.Mkfifo(p(f), 0600); err != nil {
			t.Fatal(err)
		}
	}
	ctx, cancel := context.WithTimeout(context.Background(), time.Minute)
	defer cancel()
	start := func(name, rx, tx string, port int) (*exec.Cmd, *processWait, *os.File, *os.File) {
		o, e := tempLogFilesNamed(t, name)
		c := exec.CommandContext(ctx, bin, "-x", "fifo", "-i", rx, "-o", tx, "-p", fmt.Sprint(port),
			"-b", fmt.Sprint(port+100), "-m", "1", "-C", filepath.Join(t.TempDir(), "none.ini"))
		c.Dir, c.Stdout, c.Stderr = repoRoot, o, e
		if err := startChild(c); err != nil {
			t.Fatal(err)
		}
		return c, waitForProcess(c), o, e
	}
	aPort, bPort := freePortPair(t), freePortPair(t)
	ca, pa, oa, ea := start("A", p("ar"), p("at"), aPort)
	defer func() { _ = stopProcess(t, ca, pa, oa.Name(), ea.Name()) }()
	cb, pb, ob, eb := start("B", p("br"), p("bt"), bPort)
	defer func() { _ = stopProcess(t, cb, pb, ob.Name(), eb.Name()) }()
	go pacedBridge(ctx, p("at"), p("br"), func() bool { return true })
	go pacedBridge(ctx, p("bt"), p("ar"), func() bool { return true })

	connA, err := waitForTCP(ctx, "127.0.0.1", aPort, controlPortTimeout, pa)
	if err != nil {
		t.Fatal(err)
	}
	defer connA.Close()
	connB, err := waitForTCP(ctx, "127.0.0.1", bPort, controlPortTimeout, pb)
	if err != nil {
		t.Fatal(err)
	}
	defer connB.Close()
	rwA := bufio.NewReadWriter(bufio.NewReader(connA), bufio.NewWriter(connA))
	rwB := bufio.NewReadWriter(bufio.NewReader(connB), bufio.NewWriter(connB))
	sendControlCommand(t, connA, rwA, "MYCALL TESTA")
	_, _ = rwA.ReadString('\r')
	sendControlCommand(t, connB, rwB, "MYCALL TESTB")
	_, _ = rwB.ReadString('\r')
	sendControlCommand(t, connB, rwB, "LISTEN ON")
	_ = connA.SetDeadline(time.Time{})
	_ = connB.SetDeadline(time.Now().Add(30 * time.Second))
	go func() {
		for {
			if _, err := rwA.ReadString('\r'); err != nil {
				return
			}
		}
	}()
	rwA.WriteString("CONNECT TESTA TESTB\r")
	rwA.Flush()

	// PENDING comes at the lock, about a second into the 3.7 s CALL.
	for {
		l, err := rwB.ReadString('\r')
		if err != nil {
			t.Fatalf("no PENDING: %v", err)
		}
		if strings.HasPrefix(l, "PENDING") {
			break
		}
	}
	const wait = 1200 * time.Millisecond
	time.Sleep(wait)
	rwB.WriteString("LISTEN OFF\r")
	rwB.Flush()

	re := regexp.MustCompile(`LISTEN OFF ([0-9.]+) s into a (\S+) frame`)
	deadline := time.Now().Add(5 * time.Second)
	for time.Now().Before(deadline) {
		o, _ := os.ReadFile(ob.Name())
		e, _ := os.ReadFile(eb.Name())
		if m := re.FindSubmatch(append(o, e...)); m != nil {
			age, _ := strconv.ParseFloat(string(m[1]), 64)
			if age < wait.Seconds()-0.2 || age > 3.5 {
				t.Fatalf("LISTEN OFF said %.1f s into the %s frame, want about %.1f s or a little more",
					age, m[2], wait.Seconds())
			}
			t.Logf("LISTEN OFF %.1f s into a %s frame", age, m[2])
			return
		}
		time.Sleep(50 * time.Millisecond)
	}
	printLogs(t, ob.Name(), eb.Name())
	t.Fatal("no LISTEN OFF line for the frame in flight")
}
