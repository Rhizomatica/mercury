//go:build linux

package integration

// An RTP "radio pair" for the -x rtp backend: it plays the part of two
// hermes-radio-daemons on loopback multicast.  Every 20 ms it sends each
// station an RX packet (8 kHz mono S16BE, PT 125) carrying what the other
// station is transmitting, and reads the stations' TX streams, enforcing
// the in-stream PTT contract of hermes-radio-daemon docs/RTP-AUDIO.md:
// a marker packet keys, an empty packet unkeys, 200 ms without a TX packet
// while keyed is a dead keyer.

import (
	"context"
	"encoding/binary"
	"fmt"
	"math/rand"
	"net"
	"os"
	"sync"
	"syscall"
	"time"
)

const (
	rtpPT        = 125
	rtpFrame     = 160
	rtpDataPort  = 5004
	rtpDeadKeyer = 200 * time.Millisecond
)

// rtpGroups returns the RX and TX groups for station idx (0 or 1) under a
// per-run base, so concurrent test runs never share a group.
func rtpGroups(base byte, idx int) (rx, tx net.IP) {
	return net.IPv4(239, 255, base, byte(1+2*idx)), net.IPv4(239, 255, base, byte(2+2*idx))
}

func mcastControl(group net.IP) func(network, address string, c syscall.RawConn) error {
	return func(network, address string, c syscall.RawConn) error {
		var serr error
		err := c.Control(func(fd uintptr) {
			s := int(fd)
			lo, err := net.InterfaceByName("lo")
			if err != nil {
				serr = err
				return
			}
			if serr = syscall.SetsockoptInt(s, syscall.SOL_SOCKET, syscall.SO_REUSEADDR, 1); serr != nil {
				return
			}
			mreq := &syscall.IPMreqn{Ifindex: int32(lo.Index)}
			copy(mreq.Address[:], net.IPv4(127, 0, 0, 1).To4())
			if serr = syscall.SetsockoptIPMreqn(s, syscall.IPPROTO_IP, syscall.IP_MULTICAST_IF, mreq); serr != nil {
				return
			}
			serr = syscall.SetsockoptInt(s, syscall.IPPROTO_IP, syscall.IP_MULTICAST_TTL, 0)
		})
		if err != nil {
			return err
		}
		return serr
	}
}

// listenGroup joins group on lo and binds to group:5004 itself.  Go's
// ListenPacket binds a multicast address as the wildcard, which would also
// deliver every other stream on the port (both RX groups and the other TX
// group), so the socket is made by hand.
func listenGroup(group net.IP) (net.PacketConn, error) {
	fd, err := syscall.Socket(syscall.AF_INET, syscall.SOCK_DGRAM|syscall.SOCK_CLOEXEC, 0)
	if err != nil {
		return nil, err
	}
	sa := &syscall.SockaddrInet4{Port: rtpDataPort}
	copy(sa.Addr[:], group.To4())
	lo, err := net.InterfaceByName("lo")
	if err == nil {
		err = syscall.SetsockoptInt(fd, syscall.SOL_SOCKET, syscall.SO_REUSEADDR, 1)
	}
	if err == nil {
		err = syscall.Bind(fd, sa)
	}
	if err == nil {
		mreq := &syscall.IPMreqn{Ifindex: int32(lo.Index)}
		copy(mreq.Multiaddr[:], group.To4())
		err = syscall.SetsockoptIPMreqn(fd, syscall.IPPROTO_IP, syscall.IP_ADD_MEMBERSHIP, mreq)
	}
	if err != nil {
		syscall.Close(fd)
		return nil, err
	}
	f := os.NewFile(uintptr(fd), "rtp-"+group.String())
	defer f.Close()
	return net.FilePacketConn(f)
}

type rtpStation struct {
	name   string
	rxAddr *net.UDPAddr
	rxConn net.PacketConn // sends this station's RX stream
	txConn net.PacketConn // receives its TX stream

	ssrc    uint32
	seq     uint16
	ts      uint32
	started bool

	mu        sync.Mutex
	keyed     bool
	keyedSsrc uint32
	lastTx    time.Time
	queue     []int16 // TX audio heard by the peer
	keys      int     // transmissions started (marker)
	ends      int     // ended by an empty packet
	deadKeyed int     // ended by the 200 ms watchdog
	txPkts    int     // audio packets while keyed
	rxPkts    int     // RX packets sent while keyed
	badPkts   int
}

type rtpRadioPair struct {
	st     [2]*rtpStation
	cancel context.CancelFunc
	wg     sync.WaitGroup
}

func startRTPRadioPair(ctx context.Context, base byte) (*rtpRadioPair, error) {
	ctx, cancel := context.WithCancel(ctx)
	p := &rtpRadioPair{cancel: cancel}
	for i := range p.st {
		rx, tx := rtpGroups(base, i)
		txConn, err := listenGroup(tx)
		if err != nil {
			cancel()
			return nil, fmt.Errorf("listen TX %s: %w", tx, err)
		}
		lcRx := net.ListenConfig{Control: mcastControl(rx)}
		rxConn, err := lcRx.ListenPacket(ctx, "udp4", "127.0.0.1:0")
		if err != nil {
			cancel()
			return nil, fmt.Errorf("RX socket %s: %w", rx, err)
		}
		p.st[i] = &rtpStation{
			name:   string(rune('A' + i)),
			rxAddr: &net.UDPAddr{IP: rx, Port: rtpDataPort},
			rxConn: rxConn, txConn: txConn,
			ssrc: rand.Uint32(), seq: uint16(rand.Uint32()), ts: rand.Uint32(),
		}
	}
	for i := range p.st {
		p.wg.Add(1)
		go p.readTX(ctx, p.st[i])
	}
	p.wg.Add(1)
	go p.clock(ctx)
	return p, nil
}

func (p *rtpRadioPair) readTX(ctx context.Context, s *rtpStation) {
	defer p.wg.Done()
	buf := make([]byte, 2048)
	for ctx.Err() == nil {
		_ = s.txConn.SetReadDeadline(time.Now().Add(100 * time.Millisecond))
		n, _, err := s.txConn.ReadFrom(buf)
		if err != nil {
			continue
		}
		pkt := buf[:n]
		s.mu.Lock()
		if n < 12 || pkt[0]>>6 != 2 || pkt[1]&0x7f != rtpPT || (n-12)%2 != 0 {
			s.badPkts++
			s.mu.Unlock()
			continue
		}
		marker := pkt[1]&0x80 != 0
		ssrc := binary.BigEndian.Uint32(pkt[8:12])
		payload := pkt[12:]
		switch {
		case !s.keyed && marker && len(payload) > 0:
			s.keyed, s.keyedSsrc = true, ssrc
			s.keys++
		case s.keyed && ssrc != s.keyedSsrc:
			s.mu.Unlock()
			continue // only the keying SSRC may drive the transmitter
		}
		if s.keyed {
			s.lastTx = time.Now()
			if len(payload) == 0 {
				s.keyed = false
				s.ends++
			} else {
				s.txPkts++
				for i := 0; i+1 < len(payload); i += 2 {
					s.queue = append(s.queue, int16(binary.BigEndian.Uint16(payload[i:])))
				}
			}
		}
		s.mu.Unlock()
	}
}

// clock is the radios' sample clock: one RX packet per station per 20 ms,
// on an absolute schedule.
func (p *rtpRadioPair) clock(ctx context.Context) {
	defer p.wg.Done()
	noise := rand.New(rand.NewSource(1))
	pkt := make([]byte, 12+2*rtpFrame)
	next := time.Now()
	for ctx.Err() == nil {
		next = next.Add(20 * time.Millisecond)
		time.Sleep(time.Until(next))
		for i, s := range p.st {
			peer := p.st[1-i]
			peer.mu.Lock()
			if peer.keyed && time.Since(peer.lastTx) > rtpDeadKeyer {
				peer.keyed = false
				peer.deadKeyed++
			}
			var heard []int16
			if len(peer.queue) >= rtpFrame {
				heard, peer.queue = peer.queue[:rtpFrame], peer.queue[rtpFrame:]
			} else if !peer.keyed {
				peer.queue = peer.queue[:0]
			}
			peer.mu.Unlock()

			s.mu.Lock()
			if s.keyed {
				s.rxPkts++
			}
			s.mu.Unlock()

			pkt[0] = 0x80
			pkt[1] = rtpPT
			if !s.started {
				pkt[1] |= 0x80 // first packet of the stream
				s.started = true
			}
			binary.BigEndian.PutUint16(pkt[2:], s.seq)
			binary.BigEndian.PutUint32(pkt[4:], s.ts)
			binary.BigEndian.PutUint32(pkt[8:], s.ssrc)
			for k := 0; k < rtpFrame; k++ {
				v := noise.NormFloat64() * 30 // receiver noise floor, about -60 dBFS
				if heard != nil {
					v += float64(heard[k])
				}
				if v > 32767 {
					v = 32767
				} else if v < -32768 {
					v = -32768
				}
				binary.BigEndian.PutUint16(pkt[12+2*k:], uint16(int16(v)))
			}
			_, _ = s.rxConn.WriteTo(pkt, s.rxAddr)
			s.seq++
			s.ts += rtpFrame
		}
	}
}

// Stats reports each station's PTT accounting.
func (p *rtpRadioPair) Stats() string {
	out := ""
	for _, s := range p.st {
		s.mu.Lock()
		out += fmt.Sprintf("%s: %d keyed, %d ended by empty packet, %d by dead-keyer, %d TX / %d RX packets while keyed, %d bad; ",
			s.name, s.keys, s.ends, s.deadKeyed, s.txPkts, s.rxPkts, s.badPkts)
		s.mu.Unlock()
	}
	return out
}

// Check returns an error if the PTT contract was broken.
func (p *rtpRadioPair) Check() error {
	for _, s := range p.st {
		s.mu.Lock()
		keys, ends, dead, bad := s.keys, s.ends, s.deadKeyed, s.badPkts
		tx, rx := s.txPkts, s.rxPkts
		s.mu.Unlock()
		switch {
		case keys == 0:
			return fmt.Errorf("station %s never keyed", s.name)
		case dead != 0:
			return fmt.Errorf("station %s: %d transmissions ended by the dead-keyer, not an end packet", s.name, dead)
		case ends < keys-1:
			return fmt.Errorf("station %s: %d keyed but only %d ended", s.name, keys, ends)
		case bad != 0:
			return fmt.Errorf("station %s sent %d malformed TX packets", s.name, bad)
		case rx > 0 && (tx < rx*9/10 || tx > rx*11/10+5):
			return fmt.Errorf("station %s: %d TX packets for %d RX packets while keyed (not lockstep)", s.name, tx, rx)
		}
	}
	return nil
}

func (p *rtpRadioPair) Close() {
	p.cancel()
	p.wg.Wait()
	for _, s := range p.st {
		s.rxConn.Close()
		s.txConn.Close()
	}
}
