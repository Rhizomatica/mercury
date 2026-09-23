package integration

import (
	"bufio"
	"context"
	"crypto/ecdh"
	"crypto/rand"
	"crypto/sha256"
	"encoding/hex"
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

// Encrypted ARQ sessions end to end: two real Mercury processes, bridged
// through the ch channel simulator, each with its own station key and the
// other's public key -- exactly the layout the HERMES installer writes
// (/etc/mercury/station.key, /etc/mercury/peers/<CALLSIGN>.pub).
//
// The unit tests pin the handshake and the record layer in isolation.  These
// prove the parts only a live link exercises: the negotiation bit riding the
// real CALL/ACCEPT frames, the handshake riding the first ARQ data frames,
// CONNECTED held back until the handshake completes, records surviving ARQ's
// framing, and required mode refusing a peer that will not encrypt.

type cryptoStation struct {
	call string
	priv []byte
	pub  []byte
	dir  string
}

func newCryptoStation(t *testing.T, call string) *cryptoStation {
	t.Helper()
	k, err := ecdh.X25519().GenerateKey(rand.Reader)
	if err != nil {
		t.Fatalf("X25519 key: %v", err)
	}
	dir := t.TempDir()
	if err := os.MkdirAll(filepath.Join(dir, "peers"), 0700); err != nil {
		t.Fatal(err)
	}
	s := &cryptoStation{call: call, priv: k.Bytes(), pub: k.PublicKey().Bytes(), dir: dir}
	if err := os.WriteFile(filepath.Join(dir, "station.key"), s.priv, 0600); err != nil {
		t.Fatal(err)
	}
	return s
}

// trust installs peer's public key where this station looks it up.
func (s *cryptoStation) trust(t *testing.T, peer *cryptoStation) {
	t.Helper()
	p := filepath.Join(s.dir, "peers", peer.call+".pub")
	if err := os.WriteFile(p, peer.pub, 0644); err != nil {
		t.Fatal(err)
	}
}

// ini writes a config with the given [crypto] mode and returns its path.
func (s *cryptoStation) ini(t *testing.T, mode string) string {
	t.Helper()
	p := filepath.Join(s.dir, "mercury.ini")
	body := fmt.Sprintf("[crypto]\nmode = %s\nkey_file = %s\npeers_dir = %s\n",
		mode, filepath.Join(s.dir, "station.key"), filepath.Join(s.dir, "peers"))
	if err := os.WriteFile(p, []byte(body), 0600); err != nil {
		t.Fatal(err)
	}
	return p
}

// fingerprint is what Mercury reports: the first 8 bytes of SHA-256 of the
// public key, in hex.
func fingerprint(pub []byte) string {
	h := sha256.Sum256(pub)
	return hex.EncodeToString(h[:8])
}

type cryptoLink struct {
	t            *testing.T
	connA, connB net.Conn
	rwA, rwB     *bufio.ReadWriter
	dataA, dataB net.Conn
	failWithLogs func(format string, args ...interface{})
	cleanup      []func()
}

func (l *cryptoLink) close() {
	for i := len(l.cleanup) - 1; i >= 0; i-- {
		l.cleanup[i]()
	}
}

// startCryptoLink starts A and B with the given configs, registers TESTA and
// TESTB, puts B in LISTEN and opens both data ports.
func startCryptoLink(t *testing.T, iniA, iniB string) *cryptoLink {
	repoRoot := mustRepoRoot(t)
	bin := locateOrBuildMercury(t, repoRoot)
	chBin, err := buildCh(repoRoot)
	if err != nil {
		t.Skipf("channel simulator unavailable: %v", err)
	}

	l := &cryptoLink{t: t}
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
	aPort, bPort := freePortPair(t), freePortPair(t)

	ctx, cancel := context.WithCancel(context.Background())
	l.cleanup = append(l.cleanup, cancel)

	bridge := startChannelBridge(ctx, chBin, aTX, bRX, bTX, aRX, DefaultChannelParams())
	l.cleanup = append(l.cleanup, func() { bridge.Close() })

	type inst struct {
		cmd      *exec.Cmd
		proc     *processWait
		out, err *os.File
	}
	start := func(name, rx, tx, ini string, port int) inst {
		stdout, stderr := tempLogFilesNamed(t, name)
		cmd := exec.CommandContext(ctx, bin,
			"-x", "fifo", "-i", rx, "-o", tx,
			"-p", fmt.Sprint(port), "-b", fmt.Sprint(port+100),
			"-m", "1", "-C", ini)
		cmd.Dir = repoRoot
		cmd.Stdout, cmd.Stderr = stdout, stderr
		if err := startChild(cmd); err != nil {
			t.Fatalf("start mercury %s: %v", name, err)
		}
		return inst{cmd, waitForProcess(cmd), stdout, stderr}
	}
	a := start("A", aRX, aTX, iniA, aPort)
	l.cleanup = append(l.cleanup, func() { _ = stopProcess(t, a.cmd, a.proc, a.out.Name(), a.err.Name()) })
	b := start("B", bRX, bTX, iniB, bPort)
	l.cleanup = append(l.cleanup, func() { _ = stopProcess(t, b.cmd, b.proc, b.out.Name(), b.err.Name()) })

	l.failWithLogs = func(format string, args ...interface{}) {
		printLogs(t, a.out.Name(), a.err.Name())
		printLogs(t, b.out.Name(), b.err.Name())
		l.close()
		t.Fatalf(format, args...)
	}

	if l.connA, err = waitForTCP(ctx, "127.0.0.1", aPort, controlPortTimeout, a.proc); err != nil {
		l.failWithLogs("A control port: %v", err)
	}
	if l.connB, err = waitForTCP(ctx, "127.0.0.1", bPort, controlPortTimeout, b.proc); err != nil {
		l.failWithLogs("B control port: %v", err)
	}
	l.rwA = bufio.NewReadWriter(bufio.NewReader(l.connA), bufio.NewWriter(l.connA))
	l.rwB = bufio.NewReadWriter(bufio.NewReader(l.connB), bufio.NewWriter(l.connB))

	for _, c := range []struct {
		conn  net.Conn
		rw    *bufio.ReadWriter
		cmd   string
		drain string
	}{
		{l.connA, l.rwA, "MYCALL TESTA", "REGISTERED TESTA"},
		{l.connB, l.rwB, "MYCALL TESTB", "REGISTERED TESTB"},
		{l.connB, l.rwB, "LISTEN ON", ""},
	} {
		if got := sendControlCommand(t, c.conn, c.rw, c.cmd); !strings.HasPrefix(got, "OK") {
			l.failWithLogs("%q -> %q, want OK", c.cmd, got)
		}
		if c.drain != "" {
			line, err := c.rw.ReadString('\r')
			if err != nil || !strings.HasPrefix(strings.TrimSpace(line), c.drain) {
				l.failWithLogs("after %q: got %q (%v)", c.cmd, line, err)
			}
		}
	}

	if l.dataA, err = net.DialTimeout("tcp", fmt.Sprintf("127.0.0.1:%d", aPort+1), 5*time.Second); err != nil {
		l.failWithLogs("A data port: %v", err)
	}
	if l.dataB, err = net.DialTimeout("tcp", fmt.Sprintf("127.0.0.1:%d", bPort+1), 5*time.Second); err != nil {
		l.failWithLogs("B data port: %v", err)
	}
	return l
}

// awaitLines reads control lines until one matching each wanted prefix has
// appeared, in order, or until a line in `stop` appears.  Returns every line
// seen, so a caller can assert on order as well as presence.
func awaitLines(t *testing.T, conn net.Conn, rw *bufio.ReadWriter, want []string,
	stop []string, deadline time.Duration) ([]string, bool) {
	var seen []string
	next := 0
	_ = conn.SetReadDeadline(time.Now().Add(deadline))
	defer conn.SetReadDeadline(time.Time{})
	for next < len(want) {
		line, err := rw.ReadString('\r')
		if err != nil {
			return seen, false
		}
		line = strings.TrimSpace(line)
		if line == "" || strings.HasPrefix(line, "BUFFER") || strings.HasPrefix(line, "PTT") ||
			strings.HasPrefix(line, "SN ") || strings.HasPrefix(line, "IAMALIVE") ||
			strings.HasPrefix(line, "BUSY") {
			continue
		}
		seen = append(seen, line)
		t.Logf("control: %s", line)
		if strings.HasPrefix(line, want[next]) {
			next++
			continue
		}
		for _, s := range stop {
			if strings.HasPrefix(line, s) {
				return seen, false
			}
		}
	}
	return seen, true
}

func readExactly(t *testing.T, conn net.Conn, n int, within time.Duration) []byte {
	got := make([]byte, 0, n)
	buf := make([]byte, 4096)
	deadline := time.Now().Add(within)
	for len(got) < n && time.Now().Before(deadline) {
		_ = conn.SetReadDeadline(time.Now().Add(10 * time.Second))
		k, err := conn.Read(buf)
		got = append(got, buf[:k]...)
		if err != nil {
			if ne, ok := err.(net.Error); ok && ne.Timeout() {
				continue
			}
			break
		}
	}
	return got
}

// Both stations require encryption and hold each other's key: the session
// must come up encrypted, report the right peer on each side, and carry data
// intact in both directions.
func TestMercuryARQEncrypted(t *testing.T) {
	a := newCryptoStation(t, "TESTA")
	b := newCryptoStation(t, "TESTB")
	a.trust(t, b)
	b.trust(t, a)

	l := startCryptoLink(t, a.ini(t, "required"), b.ini(t, "required"))
	defer l.close()

	if got := sendControlCommand(t, l.connA, l.rwA, "CONNECT TESTA TESTB"); !strings.HasPrefix(got, "OK") {
		l.failWithLogs("CONNECT -> %q", got)
	}

	// CONNECTED must be followed by ENCRYPTED with B's fingerprint -- and no
	// CONNECTED may appear before the handshake has finished.
	seen, ok := awaitLines(t, l.connA, l.rwA,
		[]string{"CONNECTED", "ENCRYPTED " + fingerprint(b.pub)},
		[]string{"DISCONNECTED", "CLEAR"}, 4*time.Minute)
	if !ok {
		l.failWithLogs("A: no encrypted CONNECTED; saw %q", seen)
	}
	seen, ok = awaitLines(t, l.connB, l.rwB,
		[]string{"CONNECTED", "ENCRYPTED " + fingerprint(a.pub)},
		[]string{"DISCONNECTED", "CLEAR"}, 2*time.Minute)
	if !ok {
		l.failWithLogs("B: no encrypted CONNECTED; saw %q", seen)
	}

	// Several records' worth in each direction, so records span frames.
	ab := []byte(strings.Repeat("A->B encrypted payload 0123456789. ", 30))
	ba := []byte(strings.Repeat("B->A reply, also encrypted. ", 20))
	if _, err := l.dataA.Write(ab); err != nil {
		l.failWithLogs("write A: %v", err)
	}
	if got := readExactly(t, l.dataB, len(ab), 6*time.Minute); string(got) != string(ab) {
		l.failWithLogs("A->B: got %d bytes, want %d", len(got), len(ab))
	}
	if _, err := l.dataB.Write(ba); err != nil {
		l.failWithLogs("write B: %v", err)
	}
	if got := readExactly(t, l.dataA, len(ba), 6*time.Minute); string(got) != string(ba) {
		l.failWithLogs("B->A: got %d bytes, want %d", len(got), len(ba))
	}
}

// A requires encryption; B has it off.  B answers in the clear, so A must
// hang up before ever telling its client CONNECTED: required mode never lets
// a session go clear, whichever side refuses.
func TestMercuryARQRequiredRefusesAClearPeer(t *testing.T) {
	a := newCryptoStation(t, "TESTA")
	b := newCryptoStation(t, "TESTB")
	a.trust(t, b)

	l := startCryptoLink(t, a.ini(t, "required"), b.ini(t, "off"))
	defer l.close()

	if got := sendControlCommand(t, l.connA, l.rwA, "CONNECT TESTA TESTB"); !strings.HasPrefix(got, "OK") {
		l.failWithLogs("CONNECT -> %q", got)
	}
	seen, ok := awaitLines(t, l.connA, l.rwA, []string{"DISCONNECTED"},
		[]string{"CONNECTED"}, 4*time.Minute)
	if !ok {
		l.failWithLogs("A should disconnect without CONNECTED; saw %q", seen)
	}
}

// With encryption optional on both sides but B holding no key for A, the
// session must come up in the clear -- and say so -- rather than fail.
func TestMercuryARQOptionalFallsBackToClear(t *testing.T) {
	a := newCryptoStation(t, "TESTA")
	b := newCryptoStation(t, "TESTB")
	a.trust(t, b) // A offers; B has no key for A, so B answers clear

	l := startCryptoLink(t, a.ini(t, "optional"), b.ini(t, "optional"))
	defer l.close()

	if got := sendControlCommand(t, l.connA, l.rwA, "CONNECT TESTA TESTB"); !strings.HasPrefix(got, "OK") {
		l.failWithLogs("CONNECT -> %q", got)
	}
	seen, ok := awaitLines(t, l.connA, l.rwA, []string{"CONNECTED", "CLEAR"},
		[]string{"DISCONNECTED", "ENCRYPTED"}, 4*time.Minute)
	if !ok {
		l.failWithLogs("A: expected a clear session; saw %q", seen)
	}
	msg := []byte("clear but working")
	if _, err := l.dataA.Write(msg); err != nil {
		l.failWithLogs("write: %v", err)
	}
	if got := readExactly(t, l.dataB, len(msg), 4*time.Minute); string(got) != string(msg) {
		l.failWithLogs("clear transfer: got %q", got)
	}
}
