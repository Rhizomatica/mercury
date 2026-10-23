//go:build linux

package integration

// A Hermes-Lite 2 "radio pair" for the -x hl2 backend: two emulated HL2s on
// loopback UDP, each speaking openHPSDR protocol 1 to its own Mercury.
// Each one answers discovery and start/stop, streams EP6 (126 RX samples,
// 2.625 ms) on an absolute 48 kHz clock, and reads EP2: the C&C registers
// (MOX, frequencies, drive, PA, LNA) and the TX I/Q.  While a station has
// MOX set, its TX samples go through a model of the HL2's TX FIFO (RF
// starts once 40 ms are buffered; an empty FIFO while keyed is an
// underflow; samples still buffered at unkey are lost) and come out in the
// other station's RX, 60 dB down and with noise added, when both radios
// are tuned to the same frequency.  The I/Q pass through as wire values, so
// the test checks Mercury against itself, not the sideband convention
// (tests/audioio/test_hl2_wire.c pins that).

import (
	"context"
	"encoding/binary"
	"fmt"
	"math/rand"
	"net"
	"sync"
	"time"
)

const (
	hl2PktSamples = 126
	hl2PktPeriod  = 2625 * time.Microsecond
	hl2LatencyLen = 1920 // 40 ms at 48 kHz: the FIFO priming the backend asks for
	hl2PathGain   = 1e-3 // -60 dB between the two stations
	hl2NoiseSigma = 5e-5 // per component: SNR in 3 kHz about 25 dB
)

type hl2Station struct {
	name string
	conn *net.UDPConn
	port int

	mu        sync.Mutex
	host      *net.UDPAddr // where EP6 goes once started
	streaming bool
	seq       uint32
	mox       bool
	txFreq    uint32
	rxFreq    uint32
	drive     uint8
	pa        bool
	lnaCode   uint8
	fifo      [][2]int16 // TX FIFO (wire I, Q)
	draining  bool
	air       [][2]int16 // what is on the air now, heard by the peer

	keys, unkeys  int
	moxPkts       int // EP2 packets with MOX
	moxTicks      int // EP6 ticks while MOX
	underflows    int
	lostAtUnkey   int
	discoveries   int
	starts, stops int
	badPkts       int
}

type hl2RadioPair struct {
	st     [2]*hl2Station
	cancel context.CancelFunc
	wg     sync.WaitGroup
}

func startHL2RadioPair(ctx context.Context) (*hl2RadioPair, error) {
	ctx, cancel := context.WithCancel(ctx)
	p := &hl2RadioPair{cancel: cancel}
	for i := range p.st {
		c, err := net.ListenUDP("udp4", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
		if err != nil {
			cancel()
			return nil, err
		}
		p.st[i] = &hl2Station{name: string(rune('A' + i)), conn: c,
			port: c.LocalAddr().(*net.UDPAddr).Port}
	}
	for i := range p.st {
		p.wg.Add(1)
		go p.serve(ctx, p.st[i], byte(i))
	}
	p.wg.Add(1)
	go p.clock(ctx)
	return p, nil
}

func (s *hl2Station) Addr() string { return fmt.Sprintf("127.0.0.1:%d", s.port) }

func (p *hl2RadioPair) serve(ctx context.Context, s *hl2Station, mac byte) {
	defer p.wg.Done()
	buf := make([]byte, 2048)
	for ctx.Err() == nil {
		_ = s.conn.SetReadDeadline(time.Now().Add(100 * time.Millisecond))
		n, from, err := s.conn.ReadFromUDP(buf)
		if err != nil {
			continue
		}
		pkt := buf[:n]
		if n < 4 || pkt[0] != 0xEF || pkt[1] != 0xFE {
			s.mu.Lock()
			s.badPkts++
			s.mu.Unlock()
			continue
		}
		s.mu.Lock()
		switch {
		case pkt[2] == 0x02 && n == 63: // discovery
			s.discoveries++
			reply := make([]byte, 60)
			copy(reply, []byte{0xEF, 0xFE, 0x02, 0x00, 0x1C, 0xC0, 0xA2, 0x10, mac, 73, 6})
			if s.streaming && s.host.String() != from.String() {
				reply[2] = 0x03 // busy with another host
			}
			reply[21] = 2
			_, _ = s.conn.WriteToUDP(reply, from)
		case pkt[2] == 0x04 && n == 64: // start / stop
			if pkt[3]&1 != 0 {
				s.host, s.streaming, s.seq = from, true, 0
				s.starts++
			} else {
				s.streaming = false
				s.stops++
			}
		case pkt[2] == 0x01 && pkt[3] == 0x02 && n == 1032: // EP2
			s.ep2(pkt)
		default:
			s.badPkts++
		}
		s.mu.Unlock()
	}
}

// ep2 decodes one host packet; called with s.mu held.
func (s *hl2Station) ep2(pkt []byte) {
	mox := false
	for f := 0; f < 2; f++ {
		fr := pkt[8+512*f:]
		if fr[0] != 0x7F || fr[1] != 0x7F || fr[2] != 0x7F {
			s.badPkts++
			return
		}
		c0 := fr[3]
		mox = mox || c0&1 != 0
		v := binary.BigEndian.Uint32(fr[4:8])
		switch c0 >> 1 {
		case 0x01:
			s.txFreq = v
		case 0x02:
			s.rxFreq = v
		case 0x09:
			s.drive, s.pa = fr[4], fr[5]&0x08 != 0
		case 0x0A:
			s.lnaCode = fr[7]
		}
	}
	if mox && !s.mox {
		s.keys++
		s.fifo, s.draining = s.fifo[:0], false
	}
	if !mox && s.mox {
		s.unkeys++
		s.lostAtUnkey += len(s.fifo)
		s.fifo, s.draining = s.fifo[:0], false
	}
	s.mox = mox
	if !mox {
		return
	}
	s.moxPkts++
	for f := 0; f < 2; f++ {
		fr := pkt[8+512*f+8:]
		for k := 0; k < 63; k++ {
			smp := fr[8*k:]
			if smp[0]|smp[1]|smp[2]|smp[3] != 0 { // L/R must stay 0 on an HL2
				s.badPkts++
			}
			s.fifo = append(s.fifo, [2]int16{int16(binary.BigEndian.Uint16(smp[4:])),
				int16(binary.BigEndian.Uint16(smp[6:]))})
		}
	}
	if len(s.fifo) > 3600 { // overflow: the gateware drops the excess
		s.fifo = s.fifo[len(s.fifo)-3600:]
	}
}

// clock is the radios' 48 kHz sample clock: one EP6 packet per station per
// 2.625 ms on an absolute schedule, carrying the other station's signal.
func (p *hl2RadioPair) clock(ctx context.Context) {
	defer p.wg.Done()
	noise := rand.New(rand.NewSource(1))
	pkt := make([]byte, 1032)
	start := time.Now()
	ticks := int64(0)
	for ctx.Err() == nil {
		due := int64(time.Since(start) / hl2PktPeriod)
		if due <= ticks {
			time.Sleep(500 * time.Microsecond)
			continue
		}
		if due-ticks > 40 { // fell far behind (a stalled runner): resync
			ticks = due - 1
		}
		ticks++
		// Each station's transmitter puts one packet's worth on the air.
		for _, s := range p.st {
			s.mu.Lock()
			s.air = s.air[:0]
			if s.mox {
				s.moxTicks++
				if !s.draining && len(s.fifo) >= hl2LatencyLen {
					s.draining = true
				}
				if s.draining {
					if len(s.fifo) < hl2PktSamples {
						s.underflows++
					}
					k := hl2PktSamples
					if k > len(s.fifo) {
						k = len(s.fifo)
					}
					s.air = append(s.air, s.fifo[:k]...)
					s.fifo = s.fifo[k:]
				}
			}
			s.mu.Unlock()
		}
		for i, s := range p.st {
			peer := p.st[1-i]
			peer.mu.Lock()
			heard := peer.air
			same := peer.txFreq != 0 && peer.txFreq == s.rxFreq
			peer.mu.Unlock()

			s.mu.Lock()
			if !s.streaming {
				s.mu.Unlock()
				continue
			}
			copy(pkt, []byte{0xEF, 0xFE, 0x01, 0x06})
			binary.BigEndian.PutUint32(pkt[4:], s.seq)
			s.seq++
			for f := 0; f < 2; f++ {
				fr := pkt[8+512*f:]
				fr[0], fr[1], fr[2] = 0x7F, 0x7F, 0x7F
				for j := 3; j < 8; j++ {
					fr[j] = 0
				}
				if s.mox { // address 1: forward power while keyed
					fr[3] = 1 << 3
					binary.BigEndian.PutUint16(fr[6:], 1500)
				}
				for k := 0; k < 63; k++ {
					idx := 63*f + k
					vi := noise.NormFloat64() * hl2NoiseSigma
					vq := noise.NormFloat64() * hl2NoiseSigma
					if same && idx < len(heard) {
						vi += float64(heard[idx][0]) / 32767 * hl2PathGain
						vq += float64(heard[idx][1]) / 32767 * hl2PathGain
					}
					put24(fr[8+8*k:], vi)
					put24(fr[8+8*k+3:], vq)
					fr[8+8*k+6], fr[8+8*k+7] = 0, 0
				}
			}
			host := s.host
			s.mu.Unlock()
			_, _ = s.conn.WriteToUDP(pkt, host)
		}
	}
}

func put24(b []byte, v float64) {
	x := int32(v * 8388607)
	if v >= 1 {
		x = 8388607
	} else if v <= -1 {
		x = -8388607
	}
	b[0], b[1], b[2] = byte(x>>16), byte(x>>8), byte(x)
}

// Stats reports each radio's view of its host.
func (p *hl2RadioPair) Stats() string {
	out := ""
	for _, s := range p.st {
		s.mu.Lock()
		out += fmt.Sprintf("%s: %d discoveries, %d starts, %d stops, %d keyed / %d unkeyed, "+
			"%d MOX packets / %d ticks, %d underflows, %d samples lost at unkey, "+
			"freq TX %d RX %d, drive %d, PA %v, LNA 0x%02x, %d bad; ",
			s.name, s.discoveries, s.starts, s.stops, s.keys, s.unkeys, s.moxPkts, s.moxTicks,
			s.underflows, s.lostAtUnkey, s.txFreq, s.rxFreq, s.drive, s.pa, s.lnaCode, s.badPkts)
		s.mu.Unlock()
	}
	return out
}

// Check returns an error if a radio saw its host break the protocol.
func (p *hl2RadioPair) Check(freq uint32, drive uint8) error {
	for _, s := range p.st {
		if err := s.check(freq, drive); err != nil {
			return err
		}
	}
	return nil
}

func (s *hl2Station) check(freq uint32, drive uint8) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	switch {
	case s.starts == 0:
		return fmt.Errorf("radio %s was never started", s.name)
	case s.keys == 0:
		return fmt.Errorf("radio %s never keyed", s.name)
	case s.mox:
		return fmt.Errorf("radio %s left keyed", s.name)
	case s.badPkts != 0:
		return fmt.Errorf("radio %s got %d malformed packets", s.name, s.badPkts)
	case s.txFreq != freq || s.rxFreq != freq:
		return fmt.Errorf("radio %s tuned TX %d RX %d, want %d", s.name, s.txFreq, s.rxFreq, freq)
	case s.drive != drive || !s.pa:
		return fmt.Errorf("radio %s drive %d PA %v, want %d and on", s.name, s.drive, s.pa, drive)
	case s.moxTicks > 0 && (s.moxPkts < s.moxTicks*9/10 || s.moxPkts > s.moxTicks*11/10+20):
		return fmt.Errorf("radio %s: %d MOX packets in %d ticks (not lockstep)", s.name, s.moxPkts, s.moxTicks)
	case s.underflows > s.keys*2:
		return fmt.Errorf("radio %s: TX FIFO ran dry %d times in %d transmissions", s.name, s.underflows, s.keys)
	}
	return nil
}

func (p *hl2RadioPair) Close() {
	p.cancel()
	p.wg.Wait()
	for _, s := range p.st {
		s.conn.Close()
	}
}
