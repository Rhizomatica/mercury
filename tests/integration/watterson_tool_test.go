package integration

import (
	"bytes"
	"encoding/binary"
	"math"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

// These tests cover the part of utils/watterson_test the integration bench
// depends on: its command line.  The bench passes MERCURY_CH_FADING straight
// through as --NAME and the bridge cannot read the tool's exit status while a
// run is in progress, so the tool must refuse a wrong name with a non-zero
// status and no sample output, and requireWattersonFading must catch that
// before the bench starts.  Each case runs the tool for about a millisecond.

// wattersonToolBin builds the tool once per test and returns its path.
func wattersonToolBin(t *testing.T) string {
	t.Helper()
	bin, err := buildWatterson(mustRepoRoot(t))
	if err != nil {
		t.Fatal(err)
	}
	return bin
}

// wattersonToneFile writes one second of a 1 kHz tone at 8 kHz as int16.
func wattersonToneFile(t *testing.T) string {
	t.Helper()
	const fs, n = 8000, 8000
	buf := make([]byte, 2*n)
	for i := 0; i < n; i++ {
		v := int16(3000 * math.Cos(2*math.Pi*1000*float64(i)/fs))
		binary.LittleEndian.PutUint16(buf[2*i:], uint16(v))
	}
	p := filepath.Join(t.TempDir(), "tone.s16")
	if err := os.WriteFile(p, buf, 0o644); err != nil {
		t.Fatal(err)
	}
	return p
}

type wattersonRun struct {
	exit           int
	stdout, stderr string
}

// runWatterson runs the tool with args and the given extra environment and
// returns its exit status and both streams.
func runWatterson(t *testing.T, bin string, env []string, args ...string) wattersonRun {
	t.Helper()
	cmd := exec.Command(bin, args...)
	cmd.Env = append(os.Environ(), env...)
	var out, errb bytes.Buffer
	cmd.Stdout, cmd.Stderr = &out, &errb
	err := cmd.Run()
	r := wattersonRun{stdout: out.String(), stderr: errb.String()}
	if err != nil {
		ee, ok := err.(*exec.ExitError)
		if !ok {
			t.Fatalf("%s %v: %v", filepath.Base(bin), args, err)
		}
		r.exit = ee.ExitCode()
	}
	return r
}

// TestWattersonToolListPresets: --list-presets exits 0 and prints the preset
// table to stdout, standard and legacy names included.
func TestWattersonToolListPresets(t *testing.T) {
	bin := wattersonToolBin(t)
	r := runWatterson(t, bin, nil, "--list-presets")
	if r.exit != 0 {
		t.Fatalf("--list-presets exit %d, stderr:\n%s", r.exit, r.stderr)
	}
	for _, want := range []string{"--ccir-poor", "--itur-mm", "--poor", "--mpp"} {
		if !strings.Contains(r.stdout, want+" ") {
			t.Errorf("--list-presets stdout lacks %q:\n%s", want, r.stdout)
		}
	}
	if r.stderr != "" {
		t.Errorf("--list-presets wrote to stderr:\n%s", r.stderr)
	}
}

// TestWattersonToolHelp: --help alone exits 0 and goes to stdout; no
// arguments at all is an error (exit 1, help on stderr, nothing on stdout).
func TestWattersonToolHelp(t *testing.T) {
	bin := wattersonToolBin(t)
	r := runWatterson(t, bin, nil, "--help")
	if r.exit != 0 || !strings.Contains(r.stdout, "--list-presets") || r.stderr != "" {
		t.Fatalf("--help: exit %d, stdout %d bytes, stderr %d bytes", r.exit, len(r.stdout), len(r.stderr))
	}
	r = runWatterson(t, bin, nil)
	if r.exit != 1 || r.stdout != "" || !strings.Contains(r.stderr, "usage:") {
		t.Fatalf("no arguments: exit %d, stdout %d bytes, stderr %d bytes", r.exit, len(r.stdout), len(r.stderr))
	}
}

// TestWattersonToolRejectsBadOptions: a misspelt, abbreviated or unknown
// option exits non-zero, writes no samples and creates no output file.  The
// cases include the two the audit found: an earlier draft of this change ran
// itur-mq for --itur-m, and the 1.9.18 tool exited 0 on --bogus after
// printing its usage text.
func TestWattersonToolRejectsBadOptions(t *testing.T) {
	bin := wattersonToolBin(t)
	in := wattersonToneFile(t)
	cases := []struct {
		name string
		args []string
	}{
		{"ambiguous abbreviation", []string{"--itur-m"}},
		{"unique abbreviation", []string{"--ccir-f"}},
		{"unknown option", []string{"--bogus"}},
		{"unknown preset", []string{"--preset", "bogus"}},
		{"legacy with a standard preset", []string{"--ccir-poor", "--legacy"}},
		{"standard preset with legacy", []string{"--legacy", "--ccir-poor"}},
		{"mixed spectra", []string{"--poor", "--path", "4,1,0,0.3"}},
		{"two presets", []string{"--poor", "--good"}},
		{"doppler below the floor", []string{"--path", "0,1e-8,0,0.5"}},
		{"too many paths", []string{"--path", "0,1,0,0.5", "--path", "1,1,0,0.5",
			"--path", "2,1,0,0.5", "--path", "3,1,0,0.5", "--path", "4,1,0,0.5"}},
		{"bad sample rate", []string{"--Fs", "0"}},
		{"one file", nil},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			out := filepath.Join(t.TempDir(), "out.s16")
			args := []string{in, out}
			if c.name == "one file" {
				args = []string{in}
			}
			args = append(args, c.args...)
			r := runWatterson(t, bin, nil, args...)
			if r.exit == 0 {
				t.Errorf("%v: exit 0, want non-zero", c.args)
			}
			if r.stdout != "" {
				t.Errorf("%v: wrote %d bytes to stdout, want none", c.args, len(r.stdout))
			}
			if _, err := os.Stat(out); err == nil {
				t.Errorf("%v: output file was created although the options were refused", c.args)
			}
			if r.stderr == "" {
				t.Errorf("%v: no message on stderr", c.args)
			}
		})
	}
}

// TestWattersonToolEveryPresetIsAnOption: every name --list-presets prints
// is accepted as --NAME.  The option table is built from the preset table at
// run time, so this guards the one place they could still disagree: a name
// that getopt cannot parse.
func TestWattersonToolEveryPresetIsAnOption(t *testing.T) {
	bin := wattersonToolBin(t)
	r := runWatterson(t, bin, nil, "--list-presets")
	if r.exit != 0 {
		t.Fatalf("--list-presets exit %d", r.exit)
	}
	n := 0
	for _, line := range strings.Split(r.stdout, "\n") {
		f := strings.Fields(line)
		if len(f) == 0 || !strings.HasPrefix(f[0], "--") {
			continue
		}
		name := strings.TrimPrefix(f[0], "--")
		if err := wattersonPreflight(bin, name); err != nil {
			t.Errorf("%v", err)
		}
		n++
	}
	if n < 21 {
		t.Fatalf("--list-presets printed %d presets, expected at least 21", n)
	}
}

// TestWattersonToolFsOrder: --Fs takes effect whether it comes before or
// after the preset.  Both orders must build the same channel (same output on
// the same seed), print Fs=48000 and place the 2 ms echo at 96 samples.
func TestWattersonToolFsOrder(t *testing.T) {
	bin := wattersonToolBin(t)
	in := wattersonToneFile(t)
	dir := t.TempDir()
	seed := []string{"MERCURY_WATTERSON_SEED=7"}
	outA := filepath.Join(dir, "a.s16")
	outB := filepath.Join(dir, "b.s16")
	ra := runWatterson(t, bin, seed, in, outA, "--ccir-poor", "--Fs", "48000", "--No", "-20")
	rb := runWatterson(t, bin, seed, in, outB, "--Fs", "48000", "--ccir-poor", "--No", "-20")
	for _, r := range []wattersonRun{ra, rb} {
		if r.exit != 0 {
			t.Fatalf("exit %d:\n%s", r.exit, r.stderr)
		}
		for _, want := range []string{"Fs=48000", "(96 samples = 2.0000 ms)", "spectrum=gaussian"} {
			if !strings.Contains(r.stderr, want) {
				t.Errorf("report lacks %q:\n%s", want, r.stderr)
			}
		}
	}
	a, err := os.ReadFile(outA)
	if err != nil {
		t.Fatal(err)
	}
	b, err := os.ReadFile(outB)
	if err != nil {
		t.Fatal(err)
	}
	if len(a) == 0 || !bytes.Equal(a, b) {
		t.Fatalf("outputs differ with --Fs before and after the preset (%d and %d bytes)", len(a), len(b))
	}
}

// TestWattersonToolPathReport: the path report names the spectrum and the
// realised delay.  A 0.3 ms custom delay at 8 kHz is 3 samples = 0.375 ms
// (ceil); a legacy preset's line is one sample short, as it was.
func TestWattersonToolPathReport(t *testing.T) {
	bin := wattersonToolBin(t)
	in := wattersonToneFile(t)
	out := filepath.Join(t.TempDir(), "out.s16")
	r := runWatterson(t, bin, nil, in, out, "--path", "0,0,0,0.7", "--path", "0.3,0,0,0.7")
	if r.exit != 0 || !strings.Contains(r.stderr, "delay=0.300 ms (3 samples = 0.3750 ms)") {
		t.Errorf("custom 0.3 ms path: exit %d, report:\n%s", r.exit, r.stderr)
	}
	r = runWatterson(t, bin, nil, in, out, "--poor")
	if r.exit != 0 || !strings.Contains(r.stderr, "delay=2.000 ms (15 samples = 1.8750 ms)") ||
		!strings.Contains(r.stderr, "spectrum=legacy") {
		t.Errorf("legacy --poor: exit %d, report:\n%s", r.exit, r.stderr)
	}
}

// TestWattersonPreflight: the bench's preflight accepts a real preset and
// rejects the names the tool rejects, with the tool's message attached.
func TestWattersonPreflight(t *testing.T) {
	bin := wattersonToolBin(t)
	if err := wattersonPreflight(bin, "ccir-poor"); err != nil {
		t.Fatalf("ccir-poor: %v", err)
	}
	for _, bad := range []string{"itur-m", "bogus", "ccir-f"} {
		err := wattersonPreflight(bin, bad)
		if err == nil {
			t.Errorf("%q accepted by the preflight", bad)
			continue
		}
		if !strings.Contains(err.Error(), "--"+bad) || !strings.Contains(err.Error(), "list-presets") {
			t.Errorf("%q: error does not carry the option and the tool's hint: %v", bad, err)
		}
	}
}
