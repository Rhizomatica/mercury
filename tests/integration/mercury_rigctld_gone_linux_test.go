package integration

import (
	"fmt"
	"net"
	"os"
	"syscall"
	"testing"
	"time"
)

// The rigctld host is gone, not just the old connection: a key-up times out
// on the dead session, and the reopen's new connection gets no answer either.
// Hamlib's connect has no timeout of its own (the kernel's SYN retries run
// ~135 s on Linux), so Mercury asks first, bounded, whether the host answers
// (radio_io/net_probe.h), and skips rig_open() when it does not.
//
// "Gone" is made with a listener of backlog 0 whose accept queue is full: Linux
// then drops new SYNs, so a connect gets no answer at all.  Linux only.

// backlogZeroListener listens on 127.0.0.1 with a backlog of 0.
func backlogZeroListener(t *testing.T) net.Listener {
	t.Helper()
	fd, err := syscall.Socket(syscall.AF_INET, syscall.SOCK_STREAM, 0)
	if err != nil {
		t.Fatal(err)
	}
	if err := syscall.Bind(fd, &syscall.SockaddrInet4{Addr: [4]byte{127, 0, 0, 1}}); err != nil {
		syscall.Close(fd)
		t.Fatal(err)
	}
	if err := syscall.Listen(fd, 0); err != nil {
		syscall.Close(fd)
		t.Fatal(err)
	}
	f := os.NewFile(uintptr(fd), "backlog0")
	defer f.Close()
	ln, err := net.FileListener(f)
	if err != nil {
		t.Fatal(err)
	}
	return ln
}

// vanish freezes the open links, stops accepting, and fills the accept queue
// until a connect gets no answer: from then on the host looks powered off.
func (s *silentLink) vanish(t *testing.T) {
	t.Helper()
	s.freeze()
	s.mu.Lock()
	s.paused = true
	s.mu.Unlock()
	for i := 0; i < 16; i++ {
		d, err := net.DialTimeout("tcp", s.addr(), 300*time.Millisecond)
		if err != nil {
			return // no answer: the queue is full
		}
		s.mu.Lock()
		s.conns = append(s.conns, d)
		s.mu.Unlock()
	}
	t.Fatal("the accept queue never filled")
}

// comeBack accepts and forwards again: the host is back.
func (s *silentLink) comeBack() {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.paused = false
	s.resume.Broadcast()
	for _, c := range s.held {
		c.Close()
	}
	s.held = nil
}

func TestHamlibReopenToAGoneHostIsBounded(t *testing.T) {
	var link *silentLink
	l := newRigLabVia(t, 1, 126, func(rigPort int) string { // DATAC3
		link = newSilentLinkOn(t, backlogZeroListener(t), fmt.Sprintf("127.0.0.1:%d", rigPort))
		return link.addr()
	})
	defer l.logRadioLines()

	l.transmit("before the host went away", 1, 1)

	link.vanish(t)
	l.send("while the host is gone")
	// The key-up times out on the dead session and the reopen finds nobody;
	// the unkey after it makes its own (unpaced) attempt.  Neither may block
	// in rig_open()'s connect.
	l.waitFor("the unkey's probe", func() bool { return l.count("not answering") >= 2 })
	if l.count("Hamlib session reopened") != 0 {
		l.fail("reopened a session to a host that does not answer")
	}
	lost := l.uptimeOf("Hamlib session to")
	gone := l.uptimeOf("not answering")
	if lost < 0 || gone < 0 || gone-lost > 3 {
		l.fail("the reopen to a gone host took %.1f s, want < 3 s", gone-lost)
	}

	// The host comes back: the PTT OFF left pending is delivered with no new
	// transmission, and keying resumes.
	link.comeBack()
	l.waitFor("the pending PTT OFF", func() bool { return l.count("Pending PTT OFF delivered") >= 1 })
	offs := l.count("PTT OFF via Hamlib")
	l.transmit("after the host came back", 2, offs+1)
}
