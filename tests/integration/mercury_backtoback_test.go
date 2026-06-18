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

func TestMercuryBackToBackFIFO(t *testing.T) {
	repoRoot := mustRepoRoot(t)
	bin := locateOrBuildMercury(t, repoRoot)

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
	defer func() {
		_ = stopProcess(t, cmdA, procA, outA.Name(), errA.Name())
	}()
	cmdB, procB, outB, errB := startInstance("B", bRX, bTX, bPort, bPort+100)
	defer func() {
		_ = stopProcess(t, cmdB, procB, outB.Name(), errB.Name())
	}()

	bridgeCtx, bridgeCancel := context.WithCancel(ctx)
	defer bridgeCancel()
	go bridgeDirection(bridgeCtx, aTX, bRX, "A_TX->B_RX")
	go bridgeDirection(bridgeCtx, bTX, aRX, "B_TX->A_RX")

	time.Sleep(300 * time.Millisecond)

	connA, err := waitForTCP(ctx, "127.0.0.1", aPort, controlPortTimeout, procA)
	if err != nil {
		printLogs(t, outA.Name(), errA.Name())
		printLogs(t, outB.Name(), errB.Name())
		t.Fatalf("mercury A control port: %v", err)
	}
	defer connA.Close()

	connB, err := waitForTCP(ctx, "127.0.0.1", bPort, controlPortTimeout, procB)
	if err != nil {
		printLogs(t, outA.Name(), errA.Name())
		printLogs(t, outB.Name(), errB.Name())
		t.Fatalf("mercury B control port: %v", err)
	}
	defer connB.Close()

	rwA := bufio.NewReadWriter(bufio.NewReader(connA), bufio.NewWriter(connA))
	rwB := bufio.NewReadWriter(bufio.NewReader(connB), bufio.NewWriter(connB))

	for _, tc := range []struct {
		name    string
		conn    net.Conn
		rw      *bufio.ReadWriter
		command string
		want    string
	}{
		{name: "A MYCALL", conn: connA, rw: rwA, command: "MYCALL TESTA", want: "OK"},
		{name: "B MYCALL", conn: connB, rw: rwB, command: "MYCALL TESTB", want: "OK"},
		{name: "A LISTEN ON", conn: connA, rw: rwA, command: "LISTEN ON", want: "OK"},
		{name: "B LISTEN ON", conn: connB, rw: rwB, command: "LISTEN ON", want: "OK"},
		{name: "A BUFFER", conn: connA, rw: rwA, command: "BUFFER", want: "BUFFER"},
		{name: "B BUFFER", conn: connB, rw: rwB, command: "BUFFER", want: "BUFFER"},
		{name: "A SN", conn: connA, rw: rwA, command: "SN", want: "SN"},
		{name: "B SN", conn: connB, rw: rwB, command: "SN", want: "SN"},
	} {
		t.Run(tc.name, func(t *testing.T) {
			select {
			case <-procA.done:
				t.Fatalf("mercury A exited: %v", procA.err)
			case <-procB.done:
				t.Fatalf("mercury B exited: %v", procB.err)
			default:
			}
			got := sendControlCommand(t, tc.conn, tc.rw, tc.command)
			if !strings.HasPrefix(got, tc.want) {
				printLogs(t, outA.Name(), errA.Name())
				printLogs(t, outB.Name(), errB.Name())
				t.Fatalf("%q response = %q, want prefix %q", tc.command, got, tc.want)
			}
			t.Logf("%s: %s -> %s", tc.name, tc.command, got)
		})
	}

	t.Log("bridge active; both mercury instances responding on control ports")

	bridgeCancel()

	for i, p := range []*processWait{procA, procB} {
		select {
		case <-p.done:
			t.Logf("mercury %c exited", 'A'+byte(i))
		default:
			t.Logf("mercury %c still running (bridge cancelled)", 'A'+byte(i))
		}
	}
}

func bridgeDirection(ctx context.Context, txPath, rxPath, label string) {
	for {
		rxFD, err := waitForFIFOOpen(ctx, rxPath, syscall.O_WRONLY|syscall.O_NONBLOCK)
		if err != nil {
			return
		}
		rxF := os.NewFile(uintptr(rxFD), rxPath)

		txFD, err := waitForFIFOOpen(ctx, txPath, syscall.O_RDONLY|syscall.O_NONBLOCK)
		if err != nil {
			rxF.Close()
			return
		}
		txF := os.NewFile(uintptr(txFD), txPath)

		// closer goroutine signal to unblock read on ctx.Done()
		closed := make(chan struct{})
		go func() {
			select {
			case <-ctx.Done():
				txF.Close()
				rxF.Close()
			case <-closed:
			}
		}()

		var buf [4096]byte
	readLoop:
		for {
			select {
			case <-ctx.Done():
				break readLoop
			default:
			}
			n, err := txF.Read(buf[:])
			if err != nil {
				break
			}
			if n == 0 {
				// FIFO read fd sees no writer; break and let outer loop reopen
				break
			}
			written := 0
			for written < n {
				select {
				case <-ctx.Done():
					break readLoop
				default:
				}
				wn, werr := rxF.Write(buf[written:n])
				if werr != nil {
					break readLoop
				}
				written += wn
			}
		}

		txF.Close()
		rxF.Close()
		close(closed)

		select {
		case <-ctx.Done():
			return
		default:
		}
	}
}

func waitForFIFOOpen(ctx context.Context, path string, flags int) (int, error) {
	for {
		select {
		case <-ctx.Done():
			return -1, ctx.Err()
		default:
		}
		fd, err := syscall.Open(path, flags, 0)
		if err == nil {
			return fd, nil
		}
		if err == syscall.ENXIO || err == syscall.ENOENT {
			time.Sleep(5 * time.Millisecond)
			continue
		}
		return -1, err
	}
}

func tempLogFilesNamed(t *testing.T, name string) (*os.File, *os.File) {
	t.Helper()
	dir := t.TempDir()
	if d := os.Getenv("MERCURY_TEST_LOGDIR"); d != "" {
		dir = d // persist mercury logs for debugging (not cleaned up)
	}
	stdout, err := os.Create(filepath.Join(dir, name+".stdout.log"))
	if err != nil {
		t.Fatal(err)
	}
	stderr, err := os.Create(filepath.Join(dir, name+".stderr.log"))
	if err != nil {
		_ = stdout.Close()
		t.Fatal(err)
	}
	t.Cleanup(func() {
		_ = stdout.Close()
		_ = stderr.Close()
	})
	return stdout, stderr
}
