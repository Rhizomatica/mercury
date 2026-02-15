#!/usr/bin/env python3
"""
AWGN Turboshift Test: exercises gearshift downshift and BREAK feature.

Starts commander + responder on VB-Cable with gearshift enabled.
After turboshift completes and data exchange begins at a high config,
injects configurable AWGN noise via WASAPI shared mode to corrupt the
audio channel, forcing decode failures → gearshift down → BREAK.

Usage:
  python awgn_turboshift_test.py [options]

Options:
  --noise-snr DB     Noise level in dB below signal (default: 6)
  --noise-dur SEC    Duration of noise injection in seconds (default: 60)
  --timeout SEC      Total test timeout in seconds (default: 300)
  --no-gearshift     Disable gearshift (start at fixed config)
  --config N         Starting config (default: 100 = ROBUST_0)
  --cycles N         Number of noise-on/noise-off cycles (default: 1)
  --cycle-off SEC    Seconds of silence between noise cycles (default: 30)
  --data-frames N    Data frames to wait after turboshift before noise (default: 3)

Requires: sounddevice, numpy
"""

import subprocess, socket, time, sys, threading, os, argparse
import numpy as np

MERCURY = r"C:\Program Files\Mercury\mercury.exe"
VB_OUT = "CABLE Input (VB-Audio Virtual Cable)"
VB_IN  = "CABLE Output (VB-Audio Virtual Cable)"

RSP_PORT = 7002
CMD_PORT = 7006
SAMPLE_RATE = 48000

def find_wasapi_cable_output():
    """Find the WASAPI device index for 'CABLE Input' (output direction)."""
    import sounddevice as sd
    for i, d in enumerate(sd.query_devices()):
        if ('CABLE Input' in d['name'] and
            d['max_output_channels'] >= 1 and
            d['hostapi'] == 2):  # WASAPI
            return i
    # Fallback: try any CABLE Input output device
    for i, d in enumerate(sd.query_devices()):
        if 'CABLE Input' in d['name'] and d['max_output_channels'] >= 1:
            return i
    raise RuntimeError("Could not find CABLE Input output device")


class NoiseInjector:
    """Plays AWGN noise on VB-Cable via WASAPI shared mode.

    SNR is calibrated relative to Mercury's actual passband RMS level.
    Mercury TX: PSK(±1) → IFFT(256) → /sqrt(256) → ×sqrt(2) → peak_clip
    With Nc=50: RMS ≈ sqrt(50)/sqrt(256) × sqrt(2) ≈ 0.625 → -4.1 dBFS
    """

    SIGNAL_DBFS = -4.4  # Mercury OFDM passband RMS (conservative)

    def __init__(self, device_index, snr_db=0, sample_rate=48000):
        self.device_index = device_index
        self.snr_db = snr_db
        self.sample_rate = sample_rate
        self.playing = False
        self._stream = None
        self._rng = np.random.default_rng()

        self.signal_amplitude = 10 ** (self.SIGNAL_DBFS / 20.0)
        self.noise_amplitude = self.signal_amplitude * 10 ** (-snr_db / 20.0)

    def _callback(self, outdata, frames, time_info, status):
        if self.playing:
            noise = self._rng.normal(0, self.noise_amplitude, (frames, 1)).astype(np.float32)
            outdata[:] = noise
        else:
            outdata[:] = 0

    def start(self):
        import sounddevice as sd
        self._stream = sd.OutputStream(
            device=self.device_index,
            samplerate=self.sample_rate,
            channels=1,
            dtype='float32',
            callback=self._callback,
            blocksize=1024,
        )
        self._stream.start()
        print(f"[NOISE] Stream opened on device {self.device_index}, amplitude={self.noise_amplitude:.4f} (SNR={self.snr_db}dB)")

    def noise_on(self):
        self.playing = True
        print(f"[NOISE] === ON === (amplitude={self.noise_amplitude:.4f}, SNR={self.snr_db}dB)")

    def noise_off(self):
        self.playing = False
        print(f"[NOISE] === OFF ===")

    def stop(self):
        self.playing = False
        if self._stream:
            self._stream.stop()
            self._stream.close()
            self._stream = None


def tcp_send(port, commands, label="", retries=10, delay=1):
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(5)
    for attempt in range(retries):
        try:
            sock.connect(("127.0.0.1", port))
            break
        except ConnectionRefusedError:
            if attempt == retries - 1:
                raise
            time.sleep(delay)
            sock.close()
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sock.settimeout(5)
    for cmd in commands:
        sock.sendall(cmd.encode())
        time.sleep(0.3)
        try:
            sock.settimeout(0.5)
            sock.recv(4096)
        except socket.timeout:
            pass
    return sock


def tx_thread_fn(sock, stop_event):
    chunk = bytes(range(256)) * 4
    sock.settimeout(30)
    while not stop_event.is_set():
        try:
            sock.send(chunk)
        except socket.timeout:
            continue
        except (ConnectionError, OSError):
            break
        time.sleep(0.05)


def collect_output(proc, lines_list, stop_event):
    for line in iter(proc.stdout.readline, b''):
        if stop_event.is_set():
            break
        text = line.decode('utf-8', errors='replace').rstrip()
        lines_list.append(text)


def wait_for_pattern(lines, pattern, timeout=120, poll=0.5):
    """Wait until pattern appears in lines list."""
    start = time.time()
    seen = 0
    while time.time() - start < timeout:
        while seen < len(lines):
            if pattern in lines[seen]:
                return True, lines[seen]
            seen += 1
        time.sleep(poll)
    return False, None


def count_pattern(lines, pattern, start_idx=0):
    return sum(1 for l in lines[start_idx:] if pattern in l)


def main():
    parser = argparse.ArgumentParser(description='AWGN Turboshift Test')
    parser.add_argument('--noise-snr', type=float, default=12,
                        help='Noise SNR in dB (0=equal power, negative=louder noise)')
    parser.add_argument('--noise-dur', type=float, default=90,
                        help='Duration of each noise burst in seconds')
    parser.add_argument('--timeout', type=float, default=400,
                        help='Total test timeout in seconds')
    parser.add_argument('--no-gearshift', action='store_true',
                        help='Disable gearshift')
    parser.add_argument('--config', type=int, default=100,
                        help='Starting config (default: 100=ROBUST_0)')
    parser.add_argument('--cycles', type=int, default=1,
                        help='Number of noise-on/noise-off cycles')
    parser.add_argument('--cycle-off', type=float, default=30,
                        help='Seconds of silence between noise cycles')
    parser.add_argument('--data-frames', type=int, default=3,
                        help='Data frames to wait after turboshift before noise (default: 3)')
    args = parser.parse_args()

    gearshift = not args.no_gearshift

    print(f"=== AWGN Turboshift Test ===")
    print(f"Config: {args.config}, Gearshift: {'ON' if gearshift else 'OFF'}")
    print(f"Noise: SNR={args.noise_snr}dB, duration={args.noise_dur}s, "
          f"cycles={args.cycles}, trigger after {args.data_frames} data frames")
    print(f"Timeout: {args.timeout}s")
    print()

    # Find WASAPI device
    device_idx = find_wasapi_cable_output()
    print(f"[NOISE] Using WASAPI device index {device_idx}")

    os.system("taskkill /F /IM mercury.exe 2>nul >nul")
    time.sleep(1)

    procs = []
    sockets = []
    stop_event = threading.Event()
    rsp_lines = []
    cmd_lines = []
    noise = None

    try:
        # Start noise injector (stream open but not playing yet)
        noise = NoiseInjector(device_idx, snr_db=args.noise_snr, sample_rate=SAMPLE_RATE)
        noise.start()

        # Start RSP
        rsp_cmd = [
            MERCURY, "-m", "ARQ", "-s", str(args.config), "-R",
            "-p", str(RSP_PORT),
            "-i", VB_IN, "-o", VB_OUT,
            "-x", "wasapi", "-n"
        ] + (["-g"] if gearshift else [])
        print(f"Starting responder: {' '.join(rsp_cmd)}")
        rsp = subprocess.Popen(rsp_cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        procs.append(rsp)
        threading.Thread(target=collect_output, args=(rsp, rsp_lines, stop_event), daemon=True).start()
        time.sleep(4)

        # Start CMD
        cmd_cmd = [
            MERCURY, "-m", "ARQ", "-s", str(args.config), "-R",
            "-p", str(CMD_PORT),
            "-i", VB_IN, "-o", VB_OUT,
            "-x", "wasapi", "-n"
        ] + (["-g"] if gearshift else [])
        print(f"Starting commander: {' '.join(cmd_cmd)}")
        cmd = subprocess.Popen(cmd_cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        procs.append(cmd)
        threading.Thread(target=collect_output, args=(cmd, cmd_lines, stop_event), daemon=True).start()
        time.sleep(3)

        # Setup RSP (tcp_send has 10 retries)
        rsp_sock = tcp_send(RSP_PORT, ["MYCALL TESTB\r\n", "LISTEN ON\r\n"])
        sockets.append(rsp_sock)
        time.sleep(1)

        # Data ports (with retry loop for page heap slowness)
        for attempt in range(10):
            try:
                cmd_data = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                cmd_data.settimeout(5)
                cmd_data.connect(("127.0.0.1", CMD_PORT + 1))
                break
            except (ConnectionRefusedError, socket.timeout):
                if attempt == 9: raise
                time.sleep(1)
                cmd_data.close()
        sockets.append(cmd_data)

        for attempt in range(10):
            try:
                rsp_data = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                rsp_data.settimeout(5)
                rsp_data.connect(("127.0.0.1", RSP_PORT + 1))
                break
            except (ConnectionRefusedError, socket.timeout):
                if attempt == 9: raise
                time.sleep(1)
                rsp_data.close()
        sockets.append(rsp_data)

        # TX thread
        tx_t = threading.Thread(target=tx_thread_fn, args=(cmd_data, stop_event), daemon=True)
        tx_t.start()
        time.sleep(2)

        # Connect
        cmd_sock = tcp_send(CMD_PORT, ["CONNECT TESTA TESTB\r\n"])
        sockets.append(cmd_sock)

        t_start = time.time()

        # ---- Phase 1: Wait for turboshift completion ----
        turbo_done_idx = 0  # line index in cmd_lines when turboshift completed
        if gearshift:
            print(f"\n=== Phase 1: Waiting for turboshift to complete (timeout {args.timeout}s) ===\n")
            seen_cmd = 0
            seen_rsp = 0
            turbo_done = False
            while time.time() - t_start < args.timeout:
                # Scan new commander lines for REVERSE complete
                while seen_cmd < len(cmd_lines):
                    line = cmd_lines[seen_cmd]
                    if "TURBO] REVERSE complete" in line:
                        turbo_done = True
                        turbo_done_idx = seen_cmd + 1
                        print(f"[TURBO] Complete at T+{time.time()-t_start:.1f}s: {line.strip()}")
                        break
                    elif "TURBO] FORWARD complete" in line:
                        print(f"[TURBO] Forward done at T+{time.time()-t_start:.1f}s: {line.strip()}")
                    seen_cmd += 1
                if turbo_done:
                    break

                # Also check responder (it may print the completion)
                while seen_rsp < len(rsp_lines):
                    line = rsp_lines[seen_rsp]
                    if "TURBO] REVERSE complete" in line:
                        turbo_done = True
                        turbo_done_idx = len(cmd_lines)
                        print(f"[TURBO] Complete at T+{time.time()-t_start:.1f}s (RSP): {line.strip()}")
                        break
                    seen_rsp += 1
                if turbo_done:
                    break

                # Check processes alive
                if rsp.poll() is not None or cmd.poll() is not None:
                    print("[ERROR] Process died during turboshift!")
                    break
                time.sleep(0.5)

            if not turbo_done:
                print("[ERROR] Turboshift never completed")
                print("Last CMD lines:")
                for line in cmd_lines[-20:]:
                    print(f"  {line}")
                return
        else:
            print(f"\n=== No gearshift — skipping turboshift wait ===\n")
            turbo_done_idx = len(cmd_lines)

        # ---- Phase 2: Wait for N data batches (send_batch lines) ----
        target_frames = args.data_frames
        print(f"=== Phase 2: Waiting for {target_frames} data batches before noise ===\n")
        seen_cmd = turbo_done_idx
        batch_count = 0
        while time.time() - t_start < args.timeout:
            while seen_cmd < len(cmd_lines):
                line = cmd_lines[seen_cmd]
                if "send_batch()" in line:
                    batch_count += 1
                    print(f"  [DATA] Batch {batch_count}/{target_frames}: {line.strip()}")
                    if batch_count >= target_frames:
                        break
                seen_cmd += 1
            if batch_count >= target_frames:
                break
            if rsp.poll() is not None or cmd.poll() is not None:
                print("[ERROR] Process died waiting for data batches!")
                break
            time.sleep(0.5)

        if batch_count < target_frames:
            print(f"[ERROR] Only saw {batch_count}/{target_frames} data batches")
            print("Last CMD lines:")
            for line in cmd_lines[-20:]:
                print(f"  {line}")
            return

        t_connected = time.time()
        print(f"\n[OK] Data exchange confirmed after {t_connected - t_start:.1f}s ({batch_count} batches)")

        # Report current config
        for line in reversed(cmd_lines):
            if "load_configuration" in line or "CFG" in line:
                print(f"[INFO] Last config line: {line.strip()}")
                break

        # Pre-noise stats
        pre_noise_idx = len(rsp_lines)

        # ---- Phase 3: Noise injection cycles ----
        for cycle in range(args.cycles):
            cycle_label = f"[Cycle {cycle+1}/{args.cycles}]"

            # Between cycles: recovery silence
            if cycle > 0:
                wait = args.cycle_off
                print(f"\n{cycle_label} Noise off for {wait}s (recovery period)...")
                t_wait_start = time.time()
                while time.time() - t_wait_start < wait:
                    if rsp.poll() is not None or cmd.poll() is not None:
                        print(f"  {cycle_label} Process died during recovery!")
                        break
                    time.sleep(1)
                if rsp.poll() is not None or cmd.poll() is not None:
                    break

            # Report current state
            cur_nacks = count_pattern(rsp_lines, "NAck", pre_noise_idx)
            cur_breaks = count_pattern(rsp_lines, "[BREAK]", pre_noise_idx)
            for line in reversed(cmd_lines):
                if "CFG" in line or "load_configuration" in line:
                    print(f"  {cycle_label} Current config: {line.strip()}")
                    break
            print(f"  {cycle_label} Stats since last: NAcks={cur_nacks}, BREAKs={cur_breaks}")

            # NOISE ON
            pre_noise_idx = len(rsp_lines)
            noise.noise_on()
            t_noise_start = time.time()

            # Monitor during noise
            last_report = t_noise_start
            while time.time() - t_noise_start < args.noise_dur:
                if rsp.poll() is not None or cmd.poll() is not None:
                    print(f"  {cycle_label} Process died during noise!")
                    break

                now = time.time()
                if now - last_report >= 10:
                    elapsed = now - t_noise_start
                    n_nacks = count_pattern(rsp_lines, "NAck", pre_noise_idx)
                    n_breaks_rsp = count_pattern(rsp_lines, "[BREAK]", pre_noise_idx)
                    n_breaks_cmd = count_pattern(cmd_lines, "[BREAK]", pre_noise_idx)
                    n_geardown = count_pattern(cmd_lines, "LADDER DOWN", pre_noise_idx)
                    # Find current config
                    cur_cfg = "?"
                    for line in reversed(rsp_lines):
                        if "CFG" in line and "load_configuration" in line:
                            cur_cfg = line.strip()
                            break
                    print(f"  {cycle_label} T+{elapsed:.0f}s NOISE ON | "
                          f"NAcks={n_nacks} BREAKs(rsp)={n_breaks_rsp} BREAKs(cmd)={n_breaks_cmd} "
                          f"GearDown={n_geardown} | {cur_cfg}")
                    last_report = now
                time.sleep(1)

            # NOISE OFF
            noise.noise_off()
            noise_elapsed = time.time() - t_noise_start
            n_nacks = count_pattern(rsp_lines, "NAck", pre_noise_idx)
            n_breaks_rsp = count_pattern(rsp_lines, "[BREAK]", pre_noise_idx)
            n_breaks_cmd = count_pattern(cmd_lines, "[BREAK]", pre_noise_idx)
            print(f"\n  {cycle_label} Noise off after {noise_elapsed:.1f}s | "
                  f"NAcks={n_nacks} BREAKs(rsp)={n_breaks_rsp} BREAKs(cmd)={n_breaks_cmd}")

        # ---- Post-noise monitoring ----
        print(f"\n=== Noise complete, monitoring recovery for 30s ===")
        t_post = time.time()
        while time.time() - t_post < 30:
            if rsp.poll() is not None or cmd.poll() is not None:
                break
            time.sleep(1)

        # ---- Final report ----
        t_end = time.time()
        total_elapsed = t_end - t_start

        print(f"\n{'='*70}")
        print(f"=== AWGN Turboshift Test Results ===")
        print(f"{'='*70}")
        print(f"Total runtime: {total_elapsed:.1f}s")

        # Process status
        rsp_alive = rsp.poll() is None
        cmd_alive = cmd.poll() is None
        print(f"RSP: {'alive' if rsp_alive else f'DEAD (exit={rsp.returncode})'}")
        print(f"CMD: {'alive' if cmd_alive else f'DEAD (exit={cmd.returncode})'}")

        if not rsp_alive and rsp.returncode is not None and rsp.returncode < 0:
            rc = rsp.returncode & 0xFFFFFFFF
            print(f"  RSP exit code: 0x{rc:08X}")
        if not cmd_alive and cmd.returncode is not None and cmd.returncode < 0:
            rc = cmd.returncode & 0xFFFFFFFF
            print(f"  CMD exit code: 0x{rc:08X}")

        # Count events
        total_nacks_rsp = count_pattern(rsp_lines, "NAck")
        total_nacks_cmd = count_pattern(cmd_lines, "NAck")
        total_breaks_rsp = count_pattern(rsp_lines, "[BREAK]")
        total_breaks_cmd = count_pattern(cmd_lines, "[BREAK]")
        total_config_changes = count_pattern(rsp_lines, "PHY-REINIT")
        total_geardown = count_pattern(cmd_lines, "LADDER DOWN")
        total_gearup = count_pattern(cmd_lines, "gear_shift_up")
        total_crashes_rsp = count_pattern(rsp_lines, "[CRASH]")
        total_crashes_cmd = count_pattern(cmd_lines, "[CRASH]")
        total_corrupt = count_pattern(rsp_lines, "[CORRUPT]") + count_pattern(cmd_lines, "[CORRUPT]")
        total_canary_rsp = count_pattern(rsp_lines, "[CANARY] OVERFLOW")
        total_canary_cmd = count_pattern(cmd_lines, "[CANARY] OVERFLOW")

        print(f"\nEvent counts:")
        print(f"  Config transitions: {total_config_changes}")
        print(f"  NAcks (RSP): {total_nacks_rsp}")
        print(f"  NAcks (CMD): {total_nacks_cmd}")
        print(f"  Gear down: {total_geardown}")
        print(f"  Gear up: {total_gearup}")
        print(f"  BREAK (RSP): {total_breaks_rsp}")
        print(f"  BREAK (CMD): {total_breaks_cmd}")
        print(f"  CRASH handler: {total_crashes_rsp + total_crashes_cmd}")
        print(f"  CORRUPT alerts: {total_corrupt}")
        print(f"  CANARY overflow (RSP): {total_canary_rsp}")
        print(f"  CANARY overflow (CMD): {total_canary_cmd}")

        # Print BREAK-related lines
        if total_breaks_rsp + total_breaks_cmd > 0:
            print(f"\n--- BREAK events ---")
            for line in rsp_lines:
                if "[BREAK]" in line:
                    print(f"  [RSP] {line}")
            for line in cmd_lines:
                if "[BREAK]" in line:
                    print(f"  [CMD] {line}")

        # Print crash handler output
        if total_crashes_rsp + total_crashes_cmd > 0:
            print(f"\n--- CRASH handler output ---")
            for line in rsp_lines:
                if "[CRASH]" in line:
                    print(f"  [RSP] {line}")
            for line in cmd_lines:
                if "[CRASH]" in line:
                    print(f"  [CMD] {line}")

        # Print CANARY overflow lines (the key diagnostic!)
        if total_canary_rsp + total_canary_cmd > 0:
            print(f"\n--- CANARY OVERFLOW DETECTED ---")
            for line in rsp_lines:
                if "[CANARY]" in line:
                    print(f"  [RSP] {line}")
            for line in cmd_lines:
                if "[CANARY]" in line:
                    print(f"  [CMD] {line}")

        # Save full log
        logpath = "awgn_turboshift_test.log"
        with open(logpath, "w") as f:
            f.write(f"=== AWGN Turboshift Test Log ===\n")
            f.write(f"SNR={args.noise_snr}dB dur={args.noise_dur}s cycles={args.cycles} data_frames={args.data_frames}\n\n")
            f.write("=== RSP OUTPUT ===\n")
            for line in rsp_lines:
                f.write(f"[RSP] {line}\n")
            f.write("\n=== CMD OUTPUT ===\n")
            for line in cmd_lines:
                f.write(f"[CMD] {line}\n")
        print(f"\nFull log saved to: {logpath}")

        # Verdict
        print(f"\n{'='*70}")
        if total_crashes_rsp + total_crashes_cmd > 0 or total_corrupt > 0:
            print("VERDICT: FAIL (crash or corruption detected)")
        elif not rsp_alive or not cmd_alive:
            rc = (rsp.returncode if not rsp_alive else cmd.returncode) or 0
            if rc < 0:
                print(f"VERDICT: FAIL (process crashed with 0x{rc & 0xFFFFFFFF:08X})")
            else:
                print("VERDICT: WARN (process exited unexpectedly)")
        elif total_breaks_rsp + total_breaks_cmd > 0:
            print("VERDICT: PASS (BREAK feature exercised successfully)")
        elif total_geardown > 0:
            print("VERDICT: PASS (gearshift downshift exercised)")
        else:
            print("VERDICT: PASS (but noise may not have been strong enough to trigger downshift)")
        print(f"{'='*70}")

    except Exception as e:
        import traceback
        print(f"ERROR: {e}")
        traceback.print_exc()
    finally:
        stop_event.set()
        if noise:
            noise.stop()
        for s in sockets:
            try: s.close()
            except: pass
        for p in procs:
            try: p.kill()
            except: pass
        os.system("taskkill /F /IM mercury.exe 2>nul >nul")


if __name__ == "__main__":
    main()
