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
	"runtime"
	"strings"
	"syscall"
	"testing"
	"time"
)

const (
	controlPortTimeout = 6 * time.Second
	commandTimeout     = 2 * time.Second
	// Mercury's shutdown is deliberately ordered (the main loop polls the
	// shutdown flag at 500 ms, then joins the audio threads and frees the
	// persistent FreeDV mode pool).  A plain build exits well under a second,
	// but sanitizer-instrumented builds (the ASan/TSan CI jobs) stretch that
	// same sequence past 3 s.  stopProcess waits event-driven on process
	// exit, so a generous budget costs nothing when shutdown is fast.
	processStopTimeout = 15 * time.Second
)

func TestMercurySingleProcessNullControl(t *testing.T) {
	runSingleProcessControlTest(t, []string{"-x", "null"}, "null")
}

func TestMercurySingleProcessFIFOControl(t *testing.T) {
	if runtime.GOOS == "windows" {
		t.Skip("FIFO control test requires POSIX FIFOs")
	}
	rxPath, txPath, cleanup := createFIFOAudioPaths(t)
	defer cleanup()
	runSingleProcessControlTest(t, []string{"-x", "fifo", "-i", rxPath, "-o", txPath}, "fifo")
}

func runSingleProcessControlTest(t *testing.T, audioArgs []string, backendName string) {
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
	defer cancel()

	args := append([]string{}, audioArgs...)
	args = append(args,
		"-p", fmt.Sprint(basePort),
		"-b", fmt.Sprint(broadcastPort),
		"-m", "1",
		"-C", filepath.Join(t.TempDir(), "missing-mercury.ini"),
	)
	cmd := exec.CommandContext(ctx, bin, args...)
	cmd.Dir = repoRoot
	cmd.Stdout = stdout
	cmd.Stderr = stderr

	if err := cmd.Start(); err != nil {
		t.Fatalf("start mercury -x %s: %v", backendName, err)
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
		t.Fatalf("Mercury -x %s did not open TCP control port: %v", backendName, err)
	}
	defer conn.Close()

	rw := bufio.NewReadWriter(bufio.NewReader(conn), bufio.NewWriter(conn))
	for _, tc := range []struct {
		command string
		want    string
	}{
		{command: "MYCALL TESTA", want: "OK"},
		{command: "LISTEN ON", want: "OK"},
		{command: "BUFFER", want: "BUFFER"},
	} {
		got := sendControlCommand(t, conn, rw, tc.command)
		if !strings.HasPrefix(got, tc.want) {
			printLogs(t, stdout.Name(), stderr.Name())
			t.Fatalf("%q response = %q, want prefix %q", tc.command, got, tc.want)
		}
	}

	select {
	case <-proc.done:
		printLogs(t, stdout.Name(), stderr.Name())
		t.Fatalf("mercury exited during control test: %v", proc.err)
	default:
	}

	if err := stopProcess(t, cmd, proc, stdout.Name(), stderr.Name()); err != nil {
		t.Fatal(err)
	}
}

func createFIFOAudioPaths(t *testing.T) (rxPath, txPath string, cleanup func()) {
	t.Helper()
	dir := t.TempDir()
	rxPath = filepath.Join(dir, "rx.s32le.fifo")
	txPath = filepath.Join(dir, "tx.s32le.fifo")
	for _, path := range []string{rxPath, txPath} {
		if err := syscall.Mkfifo(path, 0600); err != nil {
			t.Fatalf("mkfifo %s: %v", path, err)
		}
	}

	var keepers []*os.File
	for _, path := range []string{rxPath, txPath} {
		fd, err := syscall.Open(path, syscall.O_RDWR|syscall.O_NONBLOCK, 0600)
		if err != nil {
			for _, f := range keepers {
				_ = f.Close()
			}
			t.Fatalf("open FIFO keeper %s: %v", path, err)
		}
		keepers = append(keepers, os.NewFile(uintptr(fd), path))
	}

	return rxPath, txPath, func() {
		for _, f := range keepers {
			_ = f.Close()
		}
	}
}

type processWait struct {
	done chan struct{}
	err  error
}

func waitForProcess(cmd *exec.Cmd) *processWait {
	pw := &processWait{done: make(chan struct{})}
	go func() {
		pw.err = cmd.Wait()
		close(pw.done)
	}()
	return pw
}

func mustRepoRoot(t *testing.T) string {
	t.Helper()
	wd, err := os.Getwd()
	if err != nil {
		t.Fatal(err)
	}
	root, err := filepath.Abs(filepath.Join(wd, "..", ".."))
	if err != nil {
		t.Fatal(err)
	}
	return root
}

func locateOrBuildMercury(t *testing.T, repoRoot string) string {
	t.Helper()
	if env := os.Getenv("MERCURY_BIN"); env != "" {
		if executableExists(env) {
			return env
		}
		t.Fatalf("MERCURY_BIN is set but not executable: %s", env)
	}

	name := "mercury"
	if runtime.GOOS == "windows" {
		name = "mercury.exe"
	}
	candidate := filepath.Join(repoRoot, name)
	if executableExists(candidate) {
		return candidate
	}

	ctx, cancel := context.WithTimeout(context.Background(), 2*time.Minute)
	defer cancel()
	if _, err := exec.LookPath("make"); err != nil {
		t.Skipf("root Mercury binary not found at %s and make is not available to build it; set MERCURY_BIN or install make", candidate)
	}
	build := exec.CommandContext(ctx, "make", "-C", repoRoot)
	out, err := build.CombinedOutput()
	if err != nil {
		t.Fatalf("locate/build %s failed: %v\n%s", candidate, err, string(out))
	}
	if !executableExists(candidate) {
		t.Fatalf("build completed but binary was not found: %s", candidate)
	}
	return candidate
}

func executableExists(path string) bool {
	info, err := os.Stat(path)
	return err == nil && !info.IsDir()
}

func tempLogFiles(t *testing.T) (*os.File, *os.File) {
	t.Helper()
	dir := t.TempDir()
	stdout, err := os.Create(filepath.Join(dir, "mercury.stdout.log"))
	if err != nil {
		t.Fatal(err)
	}
	stderr, err := os.Create(filepath.Join(dir, "mercury.stderr.log"))
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

func freePort(t *testing.T) int {
	t.Helper()
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer ln.Close()
	return ln.Addr().(*net.TCPAddr).Port
}

func freePortPair(t *testing.T) int {
	t.Helper()
	for i := 0; i < 50; i++ {
		base := freePort(t)
		if base >= 65535 {
			continue
		}
		ln, err := net.Listen("tcp", fmt.Sprintf("127.0.0.1:%d", base+1))
		if err == nil {
			_ = ln.Close()
			return base
		}
	}
	t.Fatal("could not reserve adjacent TCP ports")
	return 0
}

func waitForTCP(ctx context.Context, host string, port int, timeout time.Duration, proc *processWait) (net.Conn, error) {
	deadline := time.Now().Add(timeout)
	addr := fmt.Sprintf("%s:%d", host, port)
	var lastErr error
	for time.Now().Before(deadline) {
		select {
		case <-ctx.Done():
			return nil, ctx.Err()
		case <-proc.done:
			err := proc.err
			if err == nil {
				err = errors.New("process exited")
			}
			return nil, err
		default:
		}

		conn, err := net.DialTimeout("tcp", addr, 200*time.Millisecond)
		if err == nil {
			return conn, nil
		}
		lastErr = err
		time.Sleep(100 * time.Millisecond)
	}
	return nil, fmt.Errorf("timed out waiting for %s: %w", addr, lastErr)
}

func sendControlCommand(t *testing.T, conn net.Conn, rw *bufio.ReadWriter, command string) string {
	t.Helper()
	if err := conn.SetDeadline(time.Now().Add(commandTimeout)); err != nil {
		t.Fatal(err)
	}
	if _, err := rw.WriteString(command + "\r"); err != nil {
		t.Fatalf("write %q: %v", command, err)
	}
	if err := rw.Flush(); err != nil {
		t.Fatalf("flush %q: %v", command, err)
	}
	line, err := rw.ReadString('\r')
	if err != nil {
		t.Fatalf("read response for %q: %v", command, err)
	}
	return strings.TrimSuffix(line, "\r")
}

func stopProcess(t *testing.T, cmd *exec.Cmd, proc *processWait, stdoutPath, stderrPath string) error {
	t.Helper()
	if cmd.Process == nil {
		return nil
	}

	select {
	case <-proc.done:
		return nil
	default:
	}

	if runtime.GOOS != "windows" {
		_ = cmd.Process.Signal(os.Interrupt)
	} else {
		_ = cmd.Process.Kill()
	}

	select {
	case <-proc.done:
		return nil
	case <-time.After(processStopTimeout):
		_ = cmd.Process.Kill()
		select {
		case <-proc.done:
		case <-time.After(time.Second):
		}
		printLogs(t, stdoutPath, stderrPath)
		return fmt.Errorf("mercury did not stop within %s; killed process", processStopTimeout)
	}
}

func printLogs(t *testing.T, stdoutPath, stderrPath string) {
	t.Helper()
	for _, path := range []string{stdoutPath, stderrPath} {
		f, err := os.Open(path)
		if err != nil {
			t.Logf("%s: %v", path, err)
			continue
		}
		data, _ := io.ReadAll(f)
		_ = f.Close()
		t.Logf("===== %s =====\n%s", filepath.Base(path), string(data))
	}
}
