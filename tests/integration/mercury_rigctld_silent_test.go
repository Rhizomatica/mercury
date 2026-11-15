package integration

import (
	"fmt"
	"net"
	"strconv"
	"strings"
	"sync"
	"testing"
)

// A rigctld on another host can vanish without closing the connection: the
// host loses power, its Wi-Fi drops, a NAT forgets the flow.  Nothing tells
// Mercury's socket, so writes still succeed and Hamlib's PTT call times out
// (-RIG_ETIMEOUT) instead of failing with -RIG_EIO.  The session is just as
// dead, and it has to be reopened the same way.
//
// silentLink sits between Mercury and rigctld.  freeze() makes every link
// open at that moment go silent in both directions without closing it, while
// connections made after it are forwarded as usual: the old path is dead, and
// the server is reachable again.

type silentLink struct {
	ln     net.Listener
	target string

	mu     sync.Mutex
	frozen map[net.Conn]bool // client side of each link -> silent
	conns  []net.Conn
}

func newSilentLink(t *testing.T, target string) *silentLink {
	t.Helper()
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	s := &silentLink{ln: ln, target: target, frozen: map[net.Conn]bool{}}
	t.Cleanup(s.close)
	go s.accept()
	return s
}

func (s *silentLink) addr() string { return s.ln.Addr().String() }

func (s *silentLink) accept() {
	for {
		c, err := s.ln.Accept()
		if err != nil {
			return
		}
		u, err := net.Dial("tcp", s.target)
		if err != nil {
			c.Close()
			continue
		}
		s.mu.Lock()
		s.frozen[c] = false
		s.conns = append(s.conns, c, u)
		s.mu.Unlock()
		go s.pipe(c, u, c)
		go s.pipe(u, c, c)
	}
}

// pipe copies src to dst until the link `key` is frozen; after that it keeps
// reading src and drops the bytes, so neither side sees an error or an EOF.
func (s *silentLink) pipe(dst, src, key net.Conn) {
	buf := make([]byte, 4096)
	for {
		n, err := src.Read(buf)
		if n > 0 {
			s.mu.Lock()
			silent := s.frozen[key]
			s.mu.Unlock()
			if !silent {
				if _, werr := dst.Write(buf[:n]); werr != nil {
					return
				}
			}
		}
		if err != nil {
			return
		}
	}
}

func (s *silentLink) freeze() int {
	s.mu.Lock()
	defer s.mu.Unlock()
	n := 0
	for c := range s.frozen {
		if !s.frozen[c] {
			s.frozen[c] = true
			n++
		}
	}
	return n
}

func (s *silentLink) close() {
	s.ln.Close()
	s.mu.Lock()
	defer s.mu.Unlock()
	for _, c := range s.conns {
		c.Close()
	}
}

func TestHamlibSessionSurvivesSilentRigctld(t *testing.T) {
	var link *silentLink
	l := newRigLabVia(t, 1, 126, func(rigPort int) string { // DATAC3
		link = newSilentLink(t, fmt.Sprintf("127.0.0.1:%d", rigPort))
		return link.addr()
	})
	defer l.logRadioLines()

	l.transmit("before the link went silent", 1, 1)

	if n := link.freeze(); n == 0 {
		l.fail("no rigctld link to freeze")
	}
	// The key-up times out on the silent session, reopens it over a new
	// connection and keys; the unkey goes out on the new session.
	l.transmit("after the link went silent", 2, 2)
	if l.count("Hamlib session reopened") < 1 {
		l.fail("the timed-out session was not reopened")
	}
	// Closing the silent session must not wait out another Hamlib timeout
	// for a goodbye nobody answers (10 s with netrigctl).
	lost := l.uptimeOf("Hamlib session to")
	back := l.uptimeOf("Hamlib session reopened")
	if lost < 0 || back < 0 {
		l.fail("reopen not found in the log")
	}
	if d := back - lost; d > 3 {
		l.fail("reopening the silent session took %.1f s, want < 3 s", d)
	}
}

// uptimeOf returns the "[+12.345s]" stamp of the first log line containing
// s, in seconds, or -1.
func (l *rigLab) uptimeOf(s string) float64 {
	for _, line := range strings.Split(readLog(l.t, l.logPath), "\n") {
		if !strings.Contains(line, s) {
			continue
		}
		i := strings.Index(line, "[+")
		j := strings.Index(line, "s]")
		if i < 0 || j <= i+2 {
			return -1
		}
		v, err := strconv.ParseFloat(line[i+2:j], 64)
		if err != nil {
			return -1
		}
		return v
	}
	return -1
}
