#!/usr/bin/env python3
"""
Punctured LDPC BER sweep for Mercury MFSK modes.
Runs mercury.exe -m PLOT_PASSBAND with various -P (puncture) values
and collects waterfall data.

Usage: python punctured_ber_test.py [config] [ctrl_nBits_list]
  config: 100 (ROBUST_0), 101 (ROBUST_1/2 modulation)
  ctrl_nBits_list: comma-separated, e.g. "800,1000,1200,1400,1600"

Default: sweeps config 100 with ctrl_nBits = 400,600,800,1000,1200,1400,1600
"""
import subprocess, sys, re

MERCURY = r"C:\Program Files\Mercury\mercury.exe"

config = int(sys.argv[1]) if len(sys.argv) > 1 else 100
if len(sys.argv) > 2:
    nbits_list = [int(x) for x in sys.argv[2].split(",")]
else:
    nbits_list = [400, 600, 800, 1000, 1200, 1400, 1600]

mode_name = {100: "ROBUST_0 (32-MFSK×1, rate 1/16)",
             101: "ROBUST_1 (16-MFSK×2, rate 1/16)",
             102: "ROBUST_2 (16-MFSK×2, rate 1/4)"}.get(config, f"CONFIG_{config}")

print(f"=== Punctured LDPC BER Sweep: {mode_name} ===")
print(f"ctrl_nBits values: {nbits_list}")
print()

results = {}

for nbits in nbits_list:
    label = f"P={nbits}" if nbits < 1600 else "full"
    print(f"--- Testing ctrl_nBits={nbits} ({label}) ---")

    cmd = [MERCURY, "-m", "PLOT_PASSBAND", "-s", str(config), "-R"]
    if nbits < 1600:
        cmd += ["-P", str(nbits)]

    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
        output = proc.stdout + proc.stderr
    except subprocess.TimeoutExpired:
        print(f"  TIMEOUT")
        continue

    # Parse SNR;BER lines
    curve = []
    for line in output.split("\n"):
        m = re.match(r'^(-?\d+(?:\.\d+)?);(\d+(?:\.\d+(?:e[+-]?\d+)?)?)$', line.strip())
        if m:
            snr = float(m.group(1))
            ber = float(m.group(2))
            curve.append((snr, ber))

    if not curve:
        print(f"  No BER data!")
        continue

    # Find waterfall (first SNR where BER=0)
    waterfall = None
    for snr, ber in curve:
        if ber == 0:
            waterfall = snr
            break

    # Find BER floor (min BER > 0 in the high-SNR region)
    high_snr_bers = [ber for snr, ber in curve if snr >= -5 and ber > 0]
    ber_floor = min(high_snr_bers) if high_snr_bers else 0

    results[nbits] = {"curve": curve, "waterfall": waterfall, "floor": ber_floor}

    if waterfall is not None:
        print(f"  Waterfall: {waterfall:.0f} dB  (BER floor: {ber_floor:.4f})")
    else:
        print(f"  No waterfall found! BER floor: {ber_floor:.4f}")

# Summary table
print(f"\n{'='*60}")
print(f"Summary: {mode_name}")
print(f"{'='*60}")
print(f"{'ctrl_nBits':>10} {'Eff Rate':>10} {'Waterfall':>10} {'BER Floor':>10}")
print(f"{'-'*10:>10} {'-'*10:>10} {'-'*10:>10} {'-'*10:>10}")

K = 100 if config in (100, 101) else 400  # LDPC info bits
for nbits in sorted(results.keys()):
    r = results[nbits]
    eff_rate = f"1/{nbits//K}" if nbits >= K else "N/A"
    wf = f"{r['waterfall']:.0f} dB" if r['waterfall'] is not None else "none"
    floor = f"{r['floor']:.4f}" if r['floor'] > 0 else "0"
    print(f"{nbits:>10} {eff_rate:>10} {wf:>10} {floor:>10}")

# Key SNR points
print(f"\nBER at key operating points:")
print(f"{'ctrl_nBits':>10} {'@-13dB':>8} {'@-11dB':>8} {'@-8dB':>8}")
for nbits in sorted(results.keys()):
    curve = results[nbits]["curve"]
    bers = {snr: ber for snr, ber in curve}
    b13 = f"{bers.get(-13, 'N/A'):.3f}" if -13 in bers else "N/A"
    b11 = f"{bers.get(-11, 'N/A'):.3f}" if -11 in bers else "N/A"
    b8 = f"{bers.get(-8, 'N/A'):.3f}" if -8 in bers else "N/A"
    print(f"{nbits:>10} {b13:>8} {b11:>8} {b8:>8}")
