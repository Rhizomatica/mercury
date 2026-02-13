#!/usr/bin/env python3
"""
Mercury Modem Benchmark & Stress Test Suite.

Generates VARA-style performance charts (bytes/min vs SNR) and stress-tests
the modem against adversarial noise conditions.

Sub-commands:
  sweep     Fixed-config SNR sweep — measures throughput at each SNR level per config
  stress    Random noise bursts — tests gearshift, BREAK, and recovery under adversarial noise
  adaptive  Gearshift SNR sweep — single session with gearshift, sweeps SNR down (and optionally back up)

Requires: sounddevice, numpy, matplotlib (optional, for chart generation)

Usage:
  python mercury_benchmark.py sweep [options]
  python mercury_benchmark.py stress [options]
  python mercury_benchmark.py adaptive [options]
"""
import subprocess, socket, time, sys, threading, os, argparse, csv, random
from datetime import datetime
import numpy as np

MERCURY_DEFAULT = r"C:\Program Files\Mercury\mercury.exe"
VB_OUT = "CABLE Input (VB-Audio Virtual Cable)"
VB_IN  = "CABLE Output (VB-Audio Virtual Cable)"
RSP_PORT = 7001
CMD_PORT = 7005
SAMPLE_RATE = 48000

CONFIG_NAMES = {
    100: "ROBUST_0", 101: "ROBUST_1", 102: "ROBUST_2",
    **{i: f"CONFIG_{i}" for i in range(17)}
}

# Theoretical max throughput (bps) per config — from Mercury config table
CONFIG_MAX_BPS = {
    100: 14, 101: 22, 102: 87,
    0: 84, 1: 168, 2: 252, 3: 337, 4: 421,
    5: 631, 6: 841, 7: 1051, 8: 1262, 9: 1682,
    10: 2102, 11: 2523, 12: 2943, 13: 3363,
    14: 3924, 15: 4785, 16: 5735,
}


# ============================================================
# Reused infrastructure (from awgn_turboshift_test.py)
# ============================================================

def find_wasapi_cable_output():
    """Find the WASAPI device index for 'CABLE Input' (output direction)."""
    import sounddevice as sd
    for i, d in enumerate(sd.query_devices()):
        if ('CABLE Input' in d['name'] and
            d['max_output_channels'] >= 1 and
            d['hostapi'] == 2):
            return i
    for i, d in enumerate(sd.query_devices()):
        if 'CABLE Input' in d['name'] and d['max_output_channels'] >= 1:
            return i
    raise RuntimeError("Could not find CABLE Input output device")


class NoiseInjector:
    """Plays AWGN noise on VB-Cable via WASAPI shared mode.

    SNR is calibrated relative to Mercury's actual passband RMS level.
    Mercury TX chain: PSK(±1) → IFFT(256) → /sqrt(256) → ×sqrt(2) → peak_clip
    With Nc=50 active subcarriers: RMS ≈ sqrt(50)/sqrt(256) × sqrt(2) ≈ 0.625
    Peak around -4 to -6 dBFS depending on PAPR clipping.

    signal_dbfs sets the reference level. Default -4.4 dBFS = 0.6 amplitude,
    matching the theoretical RMS. Noise amplitude = signal_amplitude × 10^(-SNR/20).
    """

    # Mercury OFDM passband RMS: sqrt(Nc)/sqrt(Nfft) * carrier_amplitude
    # = sqrt(50)/sqrt(256) * sqrt(2) ≈ 0.625 → -4.1 dBFS
    DEFAULT_SIGNAL_DBFS = -4.4  # conservative estimate accounting for peak clip

    def __init__(self, device_index, snr_db=30, sample_rate=48000, signal_dbfs=None):
        self.device_index = device_index
        self.sample_rate = sample_rate
        self.playing = False
        self._stream = None
        self._rng = np.random.default_rng()
        self.signal_dbfs = signal_dbfs if signal_dbfs is not None else self.DEFAULT_SIGNAL_DBFS
        self.signal_amplitude = 10 ** (self.signal_dbfs / 20.0)
        self.set_snr(snr_db)

    def set_snr(self, snr_db):
        self.snr_db = snr_db
        # Noise amplitude relative to actual modem signal level
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
            device=self.device_index, samplerate=self.sample_rate,
            channels=1, dtype='float32', callback=self._callback, blocksize=1024,
        )
        self._stream.start()
        print(f"[NOISE] Signal ref: {self.signal_dbfs:.1f} dBFS (amplitude={self.signal_amplitude:.4f})")
        print(f"[NOISE] SNR={self.snr_db:.1f}dB -> noise amplitude={self.noise_amplitude:.4f}")

    def noise_on(self):
        self.playing = True

    def noise_off(self):
        self.playing = False

    def stop(self):
        self.playing = False
        if self._stream:
            self._stream.stop()
            self._stream.close()
            self._stream = None


def collect_output(proc, lines_list, stop_event):
    for line in iter(proc.stdout.readline, b''):
        if stop_event.is_set():
            break
        text = line.decode('utf-8', errors='replace').rstrip()
        lines_list.append(text)


def count_pattern(lines, pattern, start_idx=0):
    return sum(1 for l in lines[start_idx:] if pattern in l)


def tcp_connect_retry(port, timeout=5, retries=10, delay=1):
    """Connect to TCP port with retries. Returns socket."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(timeout)
    for attempt in range(retries):
        try:
            sock.connect(("127.0.0.1", port))
            return sock
        except (ConnectionRefusedError, socket.timeout):
            if attempt == retries - 1:
                raise
            time.sleep(delay)
            sock.close()
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sock.settimeout(timeout)
    return sock


def tcp_send_commands(port, commands):
    """Connect to TCP control port and send commands. Returns socket."""
    sock = tcp_connect_retry(port)
    for cmd in commands:
        sock.sendall(cmd.encode())
        time.sleep(0.3)
        try:
            sock.settimeout(0.5)
            sock.recv(4096)
        except socket.timeout:
            pass
    return sock


# ============================================================
# MercurySession — manages a commander+responder pair
# ============================================================

class MercurySession:
    """Manages a commander+responder modem pair lifecycle."""

    def __init__(self, config, gearshift=False, mercury_path=MERCURY_DEFAULT):
        self.config = config
        self.gearshift = gearshift
        self.mercury_path = mercury_path
        self.robust = config >= 100

        self.procs = []
        self.sockets = []
        self.cmd_lines = []
        self.rsp_lines = []
        self.stop_event = threading.Event()
        self._tx_thread = None
        self._rx_thread = None
        self._rx_bytes = 0
        self._rx_lock = threading.Lock()
        self._rx_events = []  # (timestamp, delta_bytes, total_bytes)

    def start(self, timeout=30):
        """Launch both processes, connect TCP, start TX thread. Returns True on success."""
        os.system("taskkill /F /IM mercury.exe 2>nul >nul")
        time.sleep(1)

        robust_flag = ["-R"] if self.robust else []
        gear_flag = ["-g"] if self.gearshift else []

        # Start responder
        rsp_cmd = [
            self.mercury_path, "-m", "ARQ", "-s", str(self.config),
            *robust_flag, *gear_flag,
            "-p", str(RSP_PORT), "-i", VB_IN, "-o", VB_OUT, "-x", "wasapi", "-n"
        ]
        rsp = subprocess.Popen(rsp_cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        self.procs.append(rsp)
        threading.Thread(target=collect_output, args=(rsp, self.rsp_lines, self.stop_event), daemon=True).start()
        time.sleep(4)

        # Start commander
        cmd_cmd = [
            self.mercury_path, "-m", "ARQ", "-s", str(self.config),
            *robust_flag, *gear_flag,
            "-p", str(CMD_PORT), "-i", VB_IN, "-o", VB_OUT, "-x", "wasapi", "-n"
        ]
        cmd = subprocess.Popen(cmd_cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        self.procs.append(cmd)
        threading.Thread(target=collect_output, args=(cmd, self.cmd_lines, self.stop_event), daemon=True).start()
        time.sleep(3)

        # Setup responder
        rsp_ctrl = tcp_send_commands(RSP_PORT, ["MYCALL TESTB\r\n", "LISTEN ON\r\n"])
        self.sockets.append(rsp_ctrl)
        time.sleep(1)

        # Data ports
        cmd_data = tcp_connect_retry(CMD_PORT + 1)
        self.sockets.append(cmd_data)
        rsp_data = tcp_connect_retry(RSP_PORT + 1)
        self.sockets.append(rsp_data)

        # TX thread (pre-fill FIFO)
        self._tx_sock = cmd_data
        self._tx_thread = threading.Thread(target=self._tx_loop, daemon=True)
        self._tx_thread.start()
        time.sleep(2)

        # RX thread
        self._rx_sock = rsp_data
        self._rx_thread = threading.Thread(target=self._rx_loop, daemon=True)
        self._rx_thread.start()

        # Connect
        cmd_ctrl = tcp_send_commands(CMD_PORT, ["CONNECT TESTA TESTB\r\n"])
        self.sockets.append(cmd_ctrl)
        self._cmd_ctrl = cmd_ctrl

        return True

    def _tx_loop(self):
        chunk = bytes(range(256)) * 4
        self._tx_sock.settimeout(30)
        while not self.stop_event.is_set():
            try:
                self._tx_sock.send(chunk)
            except socket.timeout:
                continue
            except (ConnectionError, OSError):
                break
            time.sleep(0.05)

    def _rx_loop(self):
        self._rx_sock.settimeout(2)
        while not self.stop_event.is_set():
            try:
                data = self._rx_sock.recv(4096)
                if not data:
                    break
                with self._rx_lock:
                    self._rx_bytes += len(data)
                    self._rx_events.append((time.time(), len(data), self._rx_bytes))
            except socket.timeout:
                continue
            except (ConnectionError, OSError):
                break

    def wait_connected(self, timeout=120):
        """Wait for CONNECTED status on control port."""
        self._cmd_ctrl.settimeout(2)
        start = time.time()
        while time.time() - start < timeout:
            try:
                data = self._cmd_ctrl.recv(4096)
                if data and b'CONNECTED' in data:
                    return True
            except socket.timeout:
                continue
            except ConnectionError:
                return False
            if not self.is_alive():
                return False
        return False

    def wait_turboshift(self, timeout=300):
        """Wait for turboshift reverse complete."""
        start = time.time()
        seen_cmd = 0
        seen_rsp = 0
        while time.time() - start < timeout:
            while seen_cmd < len(self.cmd_lines):
                if "TURBO] REVERSE complete" in self.cmd_lines[seen_cmd]:
                    return True
                seen_cmd += 1
            while seen_rsp < len(self.rsp_lines):
                if "TURBO] REVERSE complete" in self.rsp_lines[seen_rsp]:
                    return True
                seen_rsp += 1
            if not self.is_alive():
                return False
            time.sleep(0.5)
        return False

    def wait_data_batches(self, n, timeout=120):
        """Wait for n send_batch() lines in commander output."""
        start = time.time()
        seen = 0
        count = 0
        while time.time() - start < timeout:
            while seen < len(self.cmd_lines):
                if "send_batch()" in self.cmd_lines[seen]:
                    count += 1
                    if count >= n:
                        return True
                seen += 1
            if not self.is_alive():
                return False
            time.sleep(0.5)
        return False

    def measure_throughput(self, duration_s):
        """Measure RX bytes over a time window. Returns dict with stats."""
        with self._rx_lock:
            start_bytes = self._rx_bytes
            start_events = len(self._rx_events)
        start_time = time.time()

        time.sleep(duration_s)

        end_time = time.time()
        with self._rx_lock:
            end_bytes = self._rx_bytes
            end_events = len(self._rx_events)

        rx = end_bytes - start_bytes
        dt = end_time - start_time
        bps = rx * 8 / dt if dt > 0 else 0
        bpm = rx * 60 / dt if dt > 0 else 0

        return {
            'rx_bytes': rx,
            'duration_s': dt,
            'throughput_bps': bps,
            'bytes_per_min': bpm,
            'events': end_events - start_events,
        }

    def get_rx_bytes(self):
        with self._rx_lock:
            return self._rx_bytes

    def get_stats(self, start_idx=0):
        """Parse modem stdout for event counts since start_idx."""
        return {
            'nacks_rsp': count_pattern(self.rsp_lines, "NAck", start_idx),
            'nacks_cmd': count_pattern(self.cmd_lines, "NAck", start_idx),
            'breaks_rsp': count_pattern(self.rsp_lines, "[BREAK]", start_idx),
            'breaks_cmd': count_pattern(self.cmd_lines, "[BREAK]", start_idx),
            'geardown': count_pattern(self.cmd_lines, "LADDER DOWN", start_idx),
            'gearup': count_pattern(self.cmd_lines, "LADDER UP", start_idx),
            'break_drops': count_pattern(self.cmd_lines, "Dropping", start_idx),
            'break_recovery': count_pattern(self.cmd_lines, "BREAK-RECOVERY", start_idx),
            'config_changes': count_pattern(self.rsp_lines, "PHY-REINIT", start_idx),
        }

    def get_current_config(self):
        """Find the most recent config from modem output."""
        for line in reversed(self.rsp_lines):
            if "CFG" in line and "load_configuration" in line:
                return line.strip()
        for line in reversed(self.cmd_lines):
            if "CFG" in line and "load_configuration" in line:
                return line.strip()
        return "unknown"

    def is_alive(self):
        return all(p.poll() is None for p in self.procs)

    def stop(self):
        self.stop_event.set()
        for s in self.sockets:
            try: s.close()
            except: pass
        for p in self.procs:
            try: p.kill()
            except: pass
        os.system("taskkill /F /IM mercury.exe 2>nul >nul")
        time.sleep(1)

    def save_log(self, path):
        """Save full modem output to log file."""
        with open(path, 'w') as f:
            f.write("=== RSP OUTPUT ===\n")
            for line in self.rsp_lines:
                f.write(f"[RSP] {line}\n")
            f.write("\n=== CMD OUTPUT ===\n")
            for line in self.cmd_lines:
                f.write(f"[CMD] {line}\n")


# ============================================================
# Chart generation
# ============================================================

def generate_sweep_chart(csv_path, output_path):
    """Generate VARA-style throughput vs SNR chart from sweep CSV."""
    try:
        import matplotlib
        matplotlib.use('Agg')
        import matplotlib.pyplot as plt
    except ImportError:
        print("[CHART] matplotlib not available. Install with: pip install matplotlib")
        print(f"[CHART] CSV data saved to {csv_path} — you can plot it manually.")
        return False

    # Read CSV
    configs = {}
    with open(csv_path, 'r') as f:
        reader = csv.DictReader(f)
        for row in reader:
            cfg = int(row['config'])
            name = row['config_name']
            snr = float(row['snr_db'])
            bpm = float(row['bytes_per_min'])
            if cfg not in configs:
                configs[cfg] = {'name': name, 'snr': [], 'bpm': []}
            configs[cfg]['snr'].append(snr)
            configs[cfg]['bpm'].append(bpm)

    if not configs:
        print("[CHART] No data to plot.")
        return False

    # Color palette
    colors = plt.cm.viridis(np.linspace(0, 1, len(configs)))

    fig, ax = plt.subplots(figsize=(14, 8))

    for idx, (cfg, data) in enumerate(sorted(configs.items())):
        snr = data['snr']
        bpm = data['bpm']
        # Filter out zero-throughput trailing points for cleaner lines
        last_nonzero = -1
        for i, v in enumerate(bpm):
            if v > 0:
                last_nonzero = i
        if last_nonzero < 0:
            continue
        snr_plot = snr[:last_nonzero + 2]  # Include one zero point for waterfall edge
        bpm_plot = bpm[:last_nonzero + 2]

        ax.plot(snr_plot, bpm_plot, 'o-', color=colors[idx], label=data['name'],
                linewidth=2, markersize=4)

        # Theoretical max as dashed line
        if cfg in CONFIG_MAX_BPS:
            max_bpm = CONFIG_MAX_BPS[cfg] / 8 * 60  # bps → bytes/min
            ax.axhline(y=max_bpm, color=colors[idx], linestyle='--', alpha=0.3, linewidth=1)

    ax.set_xlabel('SNR (dB)', fontsize=12)
    ax.set_ylabel('Throughput (bytes/min)', fontsize=12)
    ax.set_title('Mercury Modem — Throughput vs SNR', fontsize=14)
    ax.legend(loc='upper left', fontsize=8, ncol=2)
    ax.grid(True, alpha=0.3)
    ax.set_yscale('log')
    ax.set_xlim(right=max(s for d in configs.values() for s in d['snr']) + 2)

    plt.tight_layout()
    plt.savefig(output_path, dpi=150)
    print(f"[CHART] Saved to {output_path}")
    plt.close()
    return True


def generate_stress_chart(timeline_csv, output_path):
    """Generate stress test timeline chart."""
    try:
        import matplotlib
        matplotlib.use('Agg')
        import matplotlib.pyplot as plt
    except ImportError:
        print("[CHART] matplotlib not available.")
        return False

    timestamps = []
    rx_bytes = []
    noise_on = []

    with open(timeline_csv, 'r') as f:
        reader = csv.DictReader(f)
        for row in reader:
            timestamps.append(float(row['elapsed_s']))
            rx_bytes.append(int(row['rx_bytes_cumulative']))
            noise_on.append(int(row['noise_on']))

    if not timestamps:
        return False

    fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(14, 8), sharex=True,
                                     gridspec_kw={'height_ratios': [3, 1]})

    # Throughput (cumulative bytes)
    ax1.plot(timestamps, rx_bytes, 'b-', linewidth=1.5)
    ax1.set_ylabel('Cumulative RX Bytes', fontsize=12)
    ax1.set_title('Mercury Modem — Stress Test Timeline', fontsize=14)
    ax1.grid(True, alpha=0.3)

    # Shade noise periods
    in_noise = False
    noise_start = 0
    for i, n in enumerate(noise_on):
        if n and not in_noise:
            noise_start = timestamps[i]
            in_noise = True
        elif not n and in_noise:
            ax1.axvspan(noise_start, timestamps[i], alpha=0.15, color='red')
            ax2.axvspan(noise_start, timestamps[i], alpha=0.15, color='red')
            in_noise = False
    if in_noise:
        ax1.axvspan(noise_start, timestamps[-1], alpha=0.15, color='red')
        ax2.axvspan(noise_start, timestamps[-1], alpha=0.15, color='red')

    # Noise on/off indicator
    ax2.fill_between(timestamps, noise_on, step='post', alpha=0.5, color='red', label='Noise ON')
    ax2.set_ylabel('Noise', fontsize=12)
    ax2.set_xlabel('Time (s)', fontsize=12)
    ax2.set_yticks([0, 1])
    ax2.set_yticklabels(['OFF', 'ON'])

    plt.tight_layout()
    plt.savefig(output_path, dpi=150)
    print(f"[CHART] Saved to {output_path}")
    plt.close()
    return True


# ============================================================
# Sub-command: sweep
# ============================================================

def run_sweep(args):
    """Fixed-config SNR sweep — measures throughput at each SNR level per config."""
    configs = [int(c) for c in args.configs.split(',')]
    snr_levels = []
    snr = args.snr_start
    while snr >= args.snr_stop:
        snr_levels.append(snr)
        snr += args.snr_step  # step is negative

    ts = datetime.now().strftime('%Y%m%d_%H%M%S')
    os.makedirs(args.output_dir, exist_ok=True)
    csv_path = os.path.join(args.output_dir, f'benchmark_sweep_{ts}.csv')
    chart_path = os.path.join(args.output_dir, f'benchmark_sweep_{ts}.png')
    log_dir = os.path.join(args.output_dir, f'logs_{ts}')
    os.makedirs(log_dir, exist_ok=True)

    print(f"{'='*70}")
    print(f"Mercury Benchmark — SNR Sweep")
    print(f"{'='*70}")
    print(f"Configs: {[CONFIG_NAMES.get(c, f'CONFIG_{c}') for c in configs]}")
    print(f"SNR levels: {snr_levels} dB")
    print(f"Measurement duration: {args.measure_duration}s per point")
    print(f"Settle time: {args.settle_time}s between points")
    print(f"Total points: {len(configs)} configs x {len(snr_levels)} SNRs = {len(configs)*len(snr_levels)}")
    est_time = len(configs) * (len(snr_levels) * (args.measure_duration + args.settle_time) + 30)
    print(f"Estimated time: {est_time/3600:.1f} hours")
    print(f"Output: {csv_path}")
    print()

    # Setup noise injector
    device_idx = find_wasapi_cable_output()
    noise = NoiseInjector(device_idx, snr_db=30, sample_rate=SAMPLE_RATE,
                           signal_dbfs=args.signal_dbfs)
    noise.start()

    # CSV header
    results = []
    fieldnames = ['config', 'config_name', 'snr_db', 'rx_bytes', 'duration_s',
                  'throughput_bps', 'bytes_per_min', 'nacks', 'breaks', 'process_alive']

    try:
        for cfg_idx, cfg in enumerate(configs):
            cfg_name = CONFIG_NAMES.get(cfg, f'CONFIG_{cfg}')
            print(f"\n{'='*70}")
            print(f"[{cfg_idx+1}/{len(configs)}] Starting {cfg_name} (config={cfg})")
            print(f"{'='*70}")

            session = MercurySession(cfg, gearshift=False, mercury_path=args.mercury)
            try:
                session.start()

                if not session.wait_connected(timeout=args.timeout):
                    print(f"  [ERROR] Connection failed for {cfg_name}")
                    for snr_db in snr_levels:
                        results.append({
                            'config': cfg, 'config_name': cfg_name, 'snr_db': snr_db,
                            'rx_bytes': 0, 'duration_s': 0, 'throughput_bps': 0,
                            'bytes_per_min': 0, 'nacks': 0, 'breaks': 0, 'process_alive': False,
                        })
                    continue

                print(f"  [OK] Connected. Waiting for 2 data batches...")
                if not session.wait_data_batches(2, timeout=args.timeout):
                    print(f"  [ERROR] Data exchange not started for {cfg_name}")
                    continue

                print(f"  [OK] Data flowing. Starting SNR sweep...\n")

                zero_count = 0
                stat_baseline = len(session.rsp_lines)

                # Waterfall threshold: ROBUST modes need more patience (slow ARQ)
                if args.no_waterfall:
                    wf_threshold = 0  # disabled
                elif args.waterfall_threshold is not None:
                    wf_threshold = args.waterfall_threshold
                elif cfg >= 100:
                    wf_threshold = 4  # ROBUST modes: 4 consecutive zeros
                else:
                    wf_threshold = 2  # OFDM modes: 2 consecutive zeros

                for snr_idx, snr_db in enumerate(snr_levels):
                    if not session.is_alive():
                        print(f"  [CRASH] Process died at SNR={snr_db}dB")
                        # Record remaining as dead
                        for remaining_snr in snr_levels[snr_idx:]:
                            results.append({
                                'config': cfg, 'config_name': cfg_name, 'snr_db': remaining_snr,
                                'rx_bytes': 0, 'duration_s': 0, 'throughput_bps': 0,
                                'bytes_per_min': 0, 'nacks': 0, 'breaks': 0, 'process_alive': False,
                            })
                        break

                    noise.set_snr(snr_db)
                    noise.noise_on()

                    pre_stat = len(session.rsp_lines)
                    result = session.measure_throughput(args.measure_duration)
                    stats = session.get_stats(pre_stat)

                    noise.noise_off()

                    nacks = stats['nacks_rsp'] + stats['nacks_cmd']
                    breaks = stats['breaks_rsp'] + stats['breaks_cmd']

                    row = {
                        'config': cfg, 'config_name': cfg_name, 'snr_db': snr_db,
                        'rx_bytes': result['rx_bytes'], 'duration_s': round(result['duration_s'], 1),
                        'throughput_bps': round(result['throughput_bps'], 1),
                        'bytes_per_min': round(result['bytes_per_min'], 1),
                        'nacks': nacks, 'breaks': breaks,
                        'process_alive': session.is_alive(),
                    }
                    results.append(row)

                    status = "OK" if result['rx_bytes'] > 0 else "ZERO"
                    print(f"  [{snr_idx+1}/{len(snr_levels)}] SNR={snr_db:+.0f}dB: "
                          f"{result['bytes_per_min']:.0f} B/min "
                          f"({result['throughput_bps']:.0f} bps), "
                          f"{result['rx_bytes']}B in {result['duration_s']:.0f}s, "
                          f"NAcks={nacks} [{status}]")

                    # Waterfall detection
                    if result['rx_bytes'] == 0:
                        zero_count += 1
                        if wf_threshold > 0 and zero_count >= wf_threshold:
                            print(f"  [WATERFALL] {wf_threshold} consecutive zero-throughput points — skipping remaining SNRs")
                            for remaining_snr in snr_levels[snr_idx+1:]:
                                results.append({
                                    'config': cfg, 'config_name': cfg_name, 'snr_db': remaining_snr,
                                    'rx_bytes': 0, 'duration_s': 0, 'throughput_bps': 0,
                                    'bytes_per_min': 0, 'nacks': 0, 'breaks': 0,
                                    'process_alive': session.is_alive(),
                                })
                            break
                    else:
                        zero_count = 0

                    # Settle between points
                    if snr_idx < len(snr_levels) - 1:
                        time.sleep(args.settle_time)

            finally:
                session.save_log(os.path.join(log_dir, f'{cfg_name}.log'))
                session.stop()

        # Write CSV
        with open(csv_path, 'w', newline='') as f:
            writer = csv.DictWriter(f, fieldnames=fieldnames)
            writer.writeheader()
            writer.writerows(results)
        print(f"\n[CSV] Saved to {csv_path}")

        # Generate chart
        generate_sweep_chart(csv_path, chart_path)

        # Summary
        print(f"\n{'='*70}")
        print("SWEEP COMPLETE")
        print(f"{'='*70}")
        for cfg in configs:
            cfg_name = CONFIG_NAMES.get(cfg, f'CONFIG_{cfg}')
            cfg_rows = [r for r in results if r['config'] == cfg]
            max_bpm = max((r['bytes_per_min'] for r in cfg_rows), default=0)
            waterfall_snr = None
            for r in sorted(cfg_rows, key=lambda x: x['snr_db'], reverse=True):
                if r['bytes_per_min'] > 0:
                    waterfall_snr = r['snr_db']
            if waterfall_snr is not None:
                print(f"  {cfg_name:12s}: peak {max_bpm:.0f} B/min, waterfall ~{waterfall_snr:.0f} dB")
            else:
                print(f"  {cfg_name:12s}: no throughput measured")

    finally:
        noise.stop()


# ============================================================
# Sub-command: stress
# ============================================================

def run_stress(args):
    """Random noise bursts — tests gearshift, BREAK, and recovery."""
    rng = random.Random(args.seed if hasattr(args, 'seed') and args.seed else None)

    # Generate noise schedule
    schedule = []
    for i in range(args.num_bursts):
        dur = rng.uniform(args.min_dur, args.max_dur)
        snr = rng.uniform(args.snr_low, args.snr_high)
        silence = rng.uniform(30, 90) if i < args.num_bursts - 1 else 0
        schedule.append({'burst': i+1, 'duration': dur, 'snr_db': snr, 'silence_after': silence})

    total_noise = sum(s['duration'] for s in schedule)
    total_silence = sum(s['silence_after'] for s in schedule)
    total_est = total_noise + total_silence + 300  # +300 for turboshift + setup

    ts = datetime.now().strftime('%Y%m%d_%H%M%S')
    os.makedirs(args.output_dir, exist_ok=True)
    csv_path = os.path.join(args.output_dir, f'benchmark_stress_{ts}.csv')
    timeline_path = os.path.join(args.output_dir, f'benchmark_stress_timeline_{ts}.csv')
    chart_path = os.path.join(args.output_dir, f'benchmark_stress_{ts}.png')
    log_path = os.path.join(args.output_dir, f'benchmark_stress_{ts}.log')

    print(f"{'='*70}")
    print(f"Mercury Benchmark — Stress Test")
    print(f"{'='*70}")
    print(f"Start config: {CONFIG_NAMES.get(args.start_config, str(args.start_config))}")
    print(f"Bursts: {args.num_bursts}")
    print(f"Noise schedule:")
    for s in schedule:
        print(f"  Burst {s['burst']}: SNR={s['snr_db']:.1f}dB for {s['duration']:.0f}s, "
              f"then {s['silence_after']:.0f}s silence")
    print(f"Total noise time: {total_noise:.0f}s, silence: {total_silence:.0f}s")
    print(f"Estimated total: {total_est/60:.0f} min")
    print()

    device_idx = find_wasapi_cable_output()
    noise = NoiseInjector(device_idx, snr_db=30, sample_rate=SAMPLE_RATE,
                           signal_dbfs=args.signal_dbfs)
    noise.start()

    session = MercurySession(args.start_config, gearshift=True, mercury_path=args.mercury)
    burst_results = []
    timeline = []
    t_origin = None  # set after connection

    try:
        session.start()

        if not session.wait_connected(timeout=args.timeout):
            print("[ERROR] Connection failed")
            return

        t_origin = time.time()
        print(f"[OK] Connected at T+0s")

        # Wait for turboshift
        print("[TURBO] Waiting for turboshift to complete...")
        if not session.wait_turboshift(timeout=args.timeout):
            print("[WARN] Turboshift did not complete — proceeding anyway")
        else:
            print(f"[TURBO] Complete at T+{time.time()-t_origin:.0f}s")

        # Wait for data
        if not session.wait_data_batches(3, timeout=120):
            print("[ERROR] Data exchange not started")
            return

        print(f"[OK] Data flowing at T+{time.time()-t_origin:.0f}s")
        print(f"[OK] Config: {session.get_current_config()}")
        print()

        # Timeline recording thread
        timeline_stop = threading.Event()

        def timeline_recorder():
            while not timeline_stop.is_set():
                elapsed = time.time() - t_origin
                rx = session.get_rx_bytes()
                timeline.append({
                    'elapsed_s': round(elapsed, 1),
                    'rx_bytes_cumulative': rx,
                    'noise_on': 1 if noise.playing else 0,
                    'noise_snr': noise.snr_db if noise.playing else '',
                })
                time.sleep(1)

        tl_thread = threading.Thread(target=timeline_recorder, daemon=True)
        tl_thread.start()

        # Execute noise schedule
        for s in schedule:
            burst_num = s['burst']
            burst_snr = s['snr_db']
            burst_dur = s['duration']
            silence = s['silence_after']

            print(f"\n--- Burst {burst_num}/{args.num_bursts}: SNR={burst_snr:.1f}dB for {burst_dur:.0f}s ---")

            if not session.is_alive():
                print(f"[CRASH] Process died before burst {burst_num}")
                burst_results.append({
                    'burst': burst_num, 'snr_db': round(burst_snr, 1),
                    'duration': round(burst_dur, 0),
                    'bytes_during_noise': 0, 'bytes_during_recovery': 0,
                    'nacks': 0, 'breaks': 0, 'geardown': 0,
                    'config_at_end': 'DEAD', 'process_alive': False,
                })
                continue

            pre_stat = len(session.rsp_lines)
            pre_bytes = session.get_rx_bytes()

            # Noise ON
            noise.set_snr(burst_snr)
            noise.noise_on()
            t_burst = time.time()

            # Monitor during noise
            last_report = t_burst
            while time.time() - t_burst < burst_dur:
                if not session.is_alive():
                    print(f"  [CRASH] Process died during burst!")
                    break
                now = time.time()
                if now - last_report >= 15:
                    stats = session.get_stats(pre_stat)
                    rx_during = session.get_rx_bytes() - pre_bytes
                    print(f"  T+{now-t_origin:.0f}s | "
                          f"RX={rx_during}B NAcks={stats['nacks_rsp']+stats['nacks_cmd']} "
                          f"BREAKs={stats['breaks_rsp']+stats['breaks_cmd']} "
                          f"Drops={stats['break_drops']} "
                          f"GearDown={stats['geardown']}")
                    last_report = now
                time.sleep(1)

            # Noise OFF
            noise.noise_off()
            bytes_during_noise = session.get_rx_bytes() - pre_bytes

            print(f"  Noise off. RX during noise: {bytes_during_noise}B. "
                  f"Recovery for {silence:.0f}s...")

            # Recovery period
            pre_recovery_bytes = session.get_rx_bytes()
            t_recovery = time.time()
            while time.time() - t_recovery < silence:
                if not session.is_alive():
                    break
                time.sleep(1)
            bytes_during_recovery = session.get_rx_bytes() - pre_recovery_bytes

            stats = session.get_stats(pre_stat)
            burst_results.append({
                'burst': burst_num, 'snr_db': round(burst_snr, 1),
                'duration': round(burst_dur, 0),
                'bytes_during_noise': bytes_during_noise,
                'bytes_during_recovery': bytes_during_recovery,
                'nacks': stats['nacks_rsp'] + stats['nacks_cmd'],
                'breaks': stats['breaks_rsp'] + stats['breaks_cmd'],
                'break_drops': stats['break_drops'],
                'geardown': stats['geardown'],
                'config_at_end': session.get_current_config(),
                'process_alive': session.is_alive(),
            })

            print(f"  Burst {burst_num} done: noise={bytes_during_noise}B, "
                  f"recovery={bytes_during_recovery}B, "
                  f"NAcks={stats['nacks_rsp']+stats['nacks_cmd']}, "
                  f"BREAKs={stats['breaks_rsp']+stats['breaks_cmd']}, "
                  f"Drops={stats['break_drops']}, "
                  f"GearDown={stats['geardown']}")

        # Final recovery
        print(f"\n[POST] Final 60s recovery period...")
        time.sleep(60)

        timeline_stop.set()

        # Write CSVs
        with open(csv_path, 'w', newline='') as f:
            writer = csv.DictWriter(f, fieldnames=[
                'burst', 'snr_db', 'duration', 'bytes_during_noise',
                'bytes_during_recovery', 'nacks', 'breaks', 'break_drops',
                'geardown', 'config_at_end', 'process_alive'])
            writer.writeheader()
            writer.writerows(burst_results)

        with open(timeline_path, 'w', newline='') as f:
            writer = csv.DictWriter(f, fieldnames=[
                'elapsed_s', 'rx_bytes_cumulative', 'noise_on', 'noise_snr'])
            writer.writeheader()
            writer.writerows(timeline)

        print(f"\n[CSV] Burst data: {csv_path}")
        print(f"[CSV] Timeline:   {timeline_path}")

        generate_stress_chart(timeline_path, chart_path)

        # Summary
        total_rx = session.get_rx_bytes()
        total_stats = session.get_stats()
        print(f"\n{'='*70}")
        print(f"STRESS TEST COMPLETE")
        print(f"{'='*70}")
        print(f"  Total RX: {total_rx} bytes")
        print(f"  Bursts: {args.num_bursts}")
        print(f"  NAcks total: {total_stats['nacks_rsp']+total_stats['nacks_cmd']}")
        print(f"  BREAKs total: {total_stats['breaks_rsp']+total_stats['breaks_cmd']}")
        print(f"  BREAK drops: {total_stats['break_drops']}")
        print(f"  BREAK recovery: {total_stats['break_recovery']}")
        print(f"  Gear down (ladder): {total_stats['geardown']}")
        print(f"  Gear up (ladder): {total_stats['gearup']}")
        print(f"  Config changes: {total_stats['config_changes']}")
        print(f"  Process alive: {session.is_alive()}")

        if session.is_alive():
            print(f"\n  VERDICT: PASS")
        else:
            for p in session.procs:
                if p.returncode is not None and p.returncode < 0:
                    rc = p.returncode & 0xFFFFFFFF
                    print(f"  Exit code: 0x{rc:08X}")
            print(f"\n  VERDICT: FAIL (process crashed)")

        print(f"{'='*70}")

    finally:
        noise.stop()
        session.save_log(log_path)
        session.stop()


# ============================================================
# Sub-command: adaptive
# ============================================================

def run_adaptive(args):
    """Gearshift SNR sweep — single session, sweeps SNR down and optionally back up."""
    snr_levels_down = []
    snr = args.snr_start
    while snr >= args.snr_stop:
        snr_levels_down.append(snr)
        snr += args.snr_step

    snr_levels = list(snr_levels_down)
    if args.round_trip:
        snr_levels += list(reversed(snr_levels_down[:-1]))  # sweep back up

    ts = datetime.now().strftime('%Y%m%d_%H%M%S')
    os.makedirs(args.output_dir, exist_ok=True)
    csv_path = os.path.join(args.output_dir, f'benchmark_adaptive_{ts}.csv')
    chart_path = os.path.join(args.output_dir, f'benchmark_adaptive_{ts}.png')
    log_path = os.path.join(args.output_dir, f'benchmark_adaptive_{ts}.log')

    print(f"{'='*70}")
    print(f"Mercury Benchmark — Adaptive (Gearshift) SNR Sweep")
    print(f"{'='*70}")
    print(f"SNR levels: {snr_levels} dB")
    print(f"Measurement duration: {args.measure_duration}s per point")
    print(f"Round trip: {'yes' if args.round_trip else 'no'}")
    est_time = len(snr_levels) * (args.measure_duration + args.settle_time) + 300
    print(f"Estimated time: {est_time/60:.0f} min")
    print()

    device_idx = find_wasapi_cable_output()
    noise = NoiseInjector(device_idx, snr_db=30, sample_rate=SAMPLE_RATE,
                           signal_dbfs=args.signal_dbfs)
    noise.start()

    session = MercurySession(100, gearshift=True, mercury_path=args.mercury)
    results = []
    fieldnames = ['snr_db', 'direction', 'rx_bytes', 'duration_s', 'throughput_bps',
                  'bytes_per_min', 'nacks', 'breaks', 'geardown', 'gearup',
                  'config_at_end', 'process_alive']

    try:
        session.start()

        if not session.wait_connected(timeout=args.timeout):
            print("[ERROR] Connection failed")
            return

        t_start = time.time()
        print(f"[OK] Connected")

        print("[TURBO] Waiting for turboshift...")
        if not session.wait_turboshift(timeout=args.timeout):
            print("[WARN] Turboshift did not complete")
        else:
            print(f"[TURBO] Complete at T+{time.time()-t_start:.0f}s")

        if not session.wait_data_batches(3, timeout=120):
            print("[ERROR] Data exchange not started")
            return

        print(f"[OK] Data flowing. Config: {session.get_current_config()}")
        print()

        half = len(snr_levels_down)

        for snr_idx, snr_db in enumerate(snr_levels):
            direction = "DOWN" if snr_idx < half else "UP"

            if not session.is_alive():
                print(f"[CRASH] Process died at SNR={snr_db}dB")
                for remaining_snr in snr_levels[snr_idx:]:
                    results.append({
                        'snr_db': remaining_snr, 'direction': direction,
                        'rx_bytes': 0, 'duration_s': 0, 'throughput_bps': 0,
                        'bytes_per_min': 0, 'nacks': 0, 'breaks': 0,
                        'geardown': 0, 'gearup': 0,
                        'config_at_end': 'DEAD', 'process_alive': False,
                    })
                break

            pre_stat = len(session.rsp_lines)
            noise.set_snr(snr_db)
            noise.noise_on()

            result = session.measure_throughput(args.measure_duration)
            stats = session.get_stats(pre_stat)

            noise.noise_off()

            row = {
                'snr_db': snr_db, 'direction': direction,
                'rx_bytes': result['rx_bytes'],
                'duration_s': round(result['duration_s'], 1),
                'throughput_bps': round(result['throughput_bps'], 1),
                'bytes_per_min': round(result['bytes_per_min'], 1),
                'nacks': stats['nacks_rsp'] + stats['nacks_cmd'],
                'breaks': stats['breaks_rsp'] + stats['breaks_cmd'],
                'geardown': stats['geardown'], 'gearup': stats['gearup'],
                'config_at_end': session.get_current_config(),
                'process_alive': session.is_alive(),
            }
            results.append(row)

            print(f"  [{snr_idx+1}/{len(snr_levels)}] {direction} SNR={snr_db:+.0f}dB: "
                  f"{result['bytes_per_min']:.0f} B/min, "
                  f"GearDown={stats['geardown']} GearUp={stats['gearup']} "
                  f"NAcks={row['nacks']}")

            if snr_idx < len(snr_levels) - 1:
                time.sleep(args.settle_time)

        # Write CSV
        with open(csv_path, 'w', newline='') as f:
            writer = csv.DictWriter(f, fieldnames=fieldnames)
            writer.writeheader()
            writer.writerows(results)
        print(f"\n[CSV] Saved to {csv_path}")

        # Generate chart
        try:
            import matplotlib
            matplotlib.use('Agg')
            import matplotlib.pyplot as plt

            down_rows = [r for r in results if r['direction'] == 'DOWN' and r['process_alive']]
            up_rows = [r for r in results if r['direction'] == 'UP' and r['process_alive']]

            fig, ax = plt.subplots(figsize=(14, 8))
            if down_rows:
                ax.plot([r['snr_db'] for r in down_rows],
                        [r['bytes_per_min'] for r in down_rows],
                        'bo-', linewidth=2, markersize=5, label='SNR decreasing')
            if up_rows:
                ax.plot([r['snr_db'] for r in up_rows],
                        [r['bytes_per_min'] for r in up_rows],
                        'rs-', linewidth=2, markersize=5, label='SNR increasing')

            ax.set_xlabel('SNR (dB)', fontsize=12)
            ax.set_ylabel('Throughput (bytes/min)', fontsize=12)
            ax.set_title('Mercury Modem — Adaptive Throughput vs SNR', fontsize=14)
            ax.legend(fontsize=10)
            ax.grid(True, alpha=0.3)
            if any(r['bytes_per_min'] > 0 for r in results):
                ax.set_yscale('log')
            plt.tight_layout()
            plt.savefig(chart_path, dpi=150)
            print(f"[CHART] Saved to {chart_path}")
            plt.close()
        except ImportError:
            print("[CHART] matplotlib not available")

        # Summary
        print(f"\n{'='*70}")
        print(f"ADAPTIVE SWEEP COMPLETE")
        print(f"{'='*70}")
        total_stats = session.get_stats()
        print(f"  Process alive: {session.is_alive()}")
        print(f"  NAcks: {total_stats['nacks_rsp']+total_stats['nacks_cmd']}")
        print(f"  BREAKs: {total_stats['breaks_rsp']+total_stats['breaks_cmd']}")
        print(f"  Gear down: {total_stats['geardown']}")
        print(f"  Gear up: {total_stats['gearup']}")
        if session.is_alive():
            print(f"  VERDICT: PASS")
        else:
            print(f"  VERDICT: FAIL (process crashed)")
        print(f"{'='*70}")

    finally:
        noise.stop()
        session.save_log(log_path)
        session.stop()


# ============================================================
# CLI
# ============================================================

def main():
    parser = argparse.ArgumentParser(
        description='Mercury Modem Benchmark & Stress Test Suite',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__
    )
    parser.add_argument('--mercury', default=MERCURY_DEFAULT,
                        help=f'Path to mercury.exe (default: {MERCURY_DEFAULT})')
    parser.add_argument('--output-dir', default='./benchmark_results',
                        help='Output directory for CSV/charts/logs')
    parser.add_argument('--timeout', type=float, default=600,
                        help='Per-scenario timeout in seconds')
    parser.add_argument('--signal-dbfs', type=float,
                        default=NoiseInjector.DEFAULT_SIGNAL_DBFS,
                        help=f'Modem signal reference level in dBFS '
                             f'(default: {NoiseInjector.DEFAULT_SIGNAL_DBFS}). '
                             f'Mercury OFDM RMS is ~-4.4 dBFS with Nc=50, Nfft=256.')

    sub = parser.add_subparsers(dest='command', help='Sub-command')

    # sweep
    p_sweep = sub.add_parser('sweep', help='Fixed-config SNR sweep')
    p_sweep.add_argument('--configs', default='100,101,102,0,2,4,6,8,10,12,14,16',
                         help='Comma-separated config numbers')
    p_sweep.add_argument('--snr-start', type=float, default=30,
                         help='Starting SNR (dB)')
    p_sweep.add_argument('--snr-stop', type=float, default=-5,
                         help='Ending SNR (dB)')
    p_sweep.add_argument('--snr-step', type=float, default=-3,
                         help='SNR step (negative)')
    p_sweep.add_argument('--measure-duration', type=float, default=120,
                         help='Measurement time per SNR point (seconds)')
    p_sweep.add_argument('--settle-time', type=float, default=15,
                         help='Recovery time between SNR points (seconds)')
    p_sweep.add_argument('--waterfall-threshold', type=int, default=None,
                         help='Consecutive zero-throughput points before skipping '
                              '(default: 4 for ROBUST, 2 for OFDM). Use 0 to disable.')
    p_sweep.add_argument('--no-waterfall', action='store_true',
                         help='Disable waterfall detection entirely')

    # stress
    p_stress = sub.add_parser('stress', help='Random noise stress test')
    p_stress.add_argument('--num-bursts', type=int, default=10,
                          help='Number of noise bursts')
    p_stress.add_argument('--min-dur', type=float, default=60,
                          help='Minimum burst duration (seconds)')
    p_stress.add_argument('--max-dur', type=float, default=180,
                          help='Maximum burst duration (seconds)')
    p_stress.add_argument('--snr-low', type=float, default=-3,
                          help='Lowest burst SNR (dB)')
    p_stress.add_argument('--snr-high', type=float, default=15,
                          help='Highest burst SNR (dB)')
    p_stress.add_argument('--start-config', type=int, default=100,
                          help='Starting config (default: ROBUST_0)')
    p_stress.add_argument('--seed', type=int, default=None,
                          help='Random seed for reproducibility')

    # adaptive
    p_adaptive = sub.add_parser('adaptive', help='Gearshift SNR sweep')
    p_adaptive.add_argument('--snr-start', type=float, default=30,
                            help='Starting SNR (dB)')
    p_adaptive.add_argument('--snr-stop', type=float, default=-5,
                            help='Ending SNR (dB)')
    p_adaptive.add_argument('--snr-step', type=float, default=-3,
                            help='SNR step (negative)')
    p_adaptive.add_argument('--measure-duration', type=float, default=120,
                            help='Measurement time per SNR point (seconds)')
    p_adaptive.add_argument('--settle-time', type=float, default=15,
                            help='Recovery time between SNR points (seconds)')
    p_adaptive.add_argument('--no-round-trip', action='store_true',
                            help='Skip the sweep back up')

    args = parser.parse_args()

    if not args.command:
        parser.print_help()
        sys.exit(1)

    # Propagate common args
    args.output_dir = os.path.abspath(args.output_dir)

    if args.command == 'sweep':
        run_sweep(args)
    elif args.command == 'stress':
        run_stress(args)
    elif args.command == 'adaptive':
        args.round_trip = not args.no_round_trip
        run_adaptive(args)


if __name__ == "__main__":
    main()
