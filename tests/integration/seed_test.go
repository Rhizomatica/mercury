package integration

// The channel's random seed, in two modes.
//
// FIXED (MERCURY_TEST_SEED unset): every run of a test gets the same noise
// and fading -- the mode for benchmarks and A/B comparisons, where a change
// in the result must come from the code and not from the channel.  ch runs
// unseeded, as it always has (the C library's fixed sequence, which is what
// --seed 1 gives), so fixed-mode numbers stay comparable with earlier ones;
// watterson_test gets MERCURY_WATTERSON_SEED, or 1.
//
// RANDOM (MERCURY_TEST_SEED=random): every test draws a fresh seed -- the mode
// for finding what a fixed channel never shows.  A fixed seed hides a
// marginal decode: the connect handshake repeats to the millisecond, so a
// burst lands on the same stretch of the same noise on every run, and a
// frame that decodes there passes the test every time however close to the
// cliff it is (TestMercuryARQDisconnectReachesFloorPeer: 24 local passes on
// one stretch of noise, 2 failures in ~28 CI runs, which land elsewhere).
//
// MERCURY_TEST_SEED=<n> reruns seed n.  The seed and the rerun line are logged
// for every test, repeated on failure, and appended with the result to the
// seed history (MERCURY_TEST_SEED_LOG, default seed-history.log here), so an
// issue a random run found can be replayed later.  The rerun also needs the
// same MERCURY_* channel settings: the history line carries them.
//
// Each direction's channel gets its own seed from this one (SeedSalt).

import (
	"crypto/rand"
	"encoding/binary"
	"fmt"
	"os"
	"os/exec"
	"sort"
	"strconv"
	"strings"
	"sync"
	"testing"
	"time"
)

const (
	seedFixed  = "fixed"
	seedRandom = "random"
	seedPinned = "pinned"
)

// channelParamsFor is DefaultChannelParams with this test's seed resolved,
// logged and recorded.
func channelParamsFor(t *testing.T) ChannelParams {
	t.Helper()
	p := DefaultChannelParams()
	p.SeedMode, p.Seed = resolveSeed(t)
	return p
}

func resolveSeed(t *testing.T) (mode string, seed uint32) {
	t.Helper()
	v := strings.TrimSpace(os.Getenv("MERCURY_TEST_SEED"))
	switch v {
	case "", seedFixed:
		mode, seed = seedFixed, 1
		if w := os.Getenv("MERCURY_WATTERSON_SEED"); w != "" {
			if n, err := strconv.ParseUint(w, 0, 32); err == nil {
				seed = uint32(n)
			}
		}
	case seedRandom:
		var b [4]byte
		if _, err := rand.Read(b[:]); err != nil {
			t.Fatalf("MERCURY_TEST_SEED=random: %v", err)
		}
		mode, seed = seedRandom, binary.LittleEndian.Uint32(b[:])
	default:
		n, err := strconv.ParseUint(v, 0, 32)
		if err != nil {
			t.Fatalf("bad MERCURY_TEST_SEED %q: want random, fixed or a number", v)
		}
		mode, seed = seedPinned, uint32(n)
	}

	rerun := fmt.Sprintf("MERCURY_TEST_SEED=%d go test -run '^%s$'", seed, t.Name())
	if mode == seedFixed {
		rerun = fmt.Sprintf("go test -run '^%s$'", t.Name())
	}
	t.Logf("channel seed: %s %d (rerun: %s)", mode, seed, rerun)
	t.Cleanup(func() {
		result := "PASS"
		if t.Failed() {
			result = "FAIL"
			t.Logf("channel seed: %s %d -- reproduce with: %s %s", mode, seed, settingsLine(), rerun)
		} else if t.Skipped() {
			result = "SKIP"
		}
		appendSeedHistory(fmt.Sprintf("%s %s %s %s %d %s %s",
			time.Now().UTC().Format(time.RFC3339), gitHead(), t.Name(), mode, seed, result, settingsLine()))
	})
	return mode, seed
}

// derivedSeed is the seed of one direction's channel process.
func (p ChannelParams) derivedSeed() uint32 {
	return p.Seed*2654435761 + p.SeedSalt
}

// seedArgsEnv gives a channel process its seed: ch on the command line,
// watterson_test in the environment.  Fixed mode leaves ch unseeded, as it
// always was; pathsim has no seed input.
func (p ChannelParams) seedArgsEnv() (args []string, env []string) {
	switch p.Engine {
	case "watterson":
		if p.SeedMode == "" {
			// Params built without channelParamsFor: the old rule, a pinned
			// MERCURY_WATTERSON_SEED salted per direction, else the tool's own.
			n, err := strconv.ParseUint(os.Getenv("MERCURY_WATTERSON_SEED"), 0, 32)
			if err != nil || p.SeedSalt == 0 {
				return nil, nil
			}
			return nil, append(os.Environ(), fmt.Sprintf("MERCURY_WATTERSON_SEED=%d",
				uint32(n)*2654435761+p.SeedSalt))
		}
		return nil, append(os.Environ(), fmt.Sprintf("MERCURY_WATTERSON_SEED=%d", p.derivedSeed()))
	case "pathsim":
		return nil, nil
	default:
		if p.SeedMode == "" || p.SeedMode == seedFixed {
			return nil, nil
		}
		return []string{"--seed", fmt.Sprint(p.derivedSeed())}, nil
	}
}

// The MERCURY_* settings a rerun needs (paths excluded).
func settingsLine() string {
	var kv []string
	for _, e := range os.Environ() {
		if !strings.HasPrefix(e, "MERCURY_") || strings.HasPrefix(e, "MERCURY_TEST_SEED") {
			continue
		}
		k := e[:strings.IndexByte(e, '=')]
		if strings.HasSuffix(k, "_BIN") || strings.Contains(k, "_BIN_") || strings.HasSuffix(k, "LOGDIR") {
			continue
		}
		kv = append(kv, e)
	}
	sort.Strings(kv)
	return strings.Join(kv, " ")
}

var (
	seedLogMu sync.Mutex
	headOnce  sync.Once
	headHash  string
)

func gitHead() string {
	headOnce.Do(func() {
		out, err := exec.Command("git", "rev-parse", "--short", "HEAD").Output()
		headHash = "unknown"
		if err == nil {
			headHash = strings.TrimSpace(string(out))
		}
	})
	return headHash
}

func appendSeedHistory(line string) {
	path := os.Getenv("MERCURY_TEST_SEED_LOG")
	if path == "" {
		path = "seed-history.log"
	}
	seedLogMu.Lock()
	defer seedLogMu.Unlock()
	f, err := os.OpenFile(path, os.O_CREATE|os.O_APPEND|os.O_WRONLY, 0644)
	if err != nil {
		fmt.Fprintf(os.Stderr, "seed history %s: %v\n", path, err)
		return
	}
	defer f.Close()
	fmt.Fprintln(f, line)
}
