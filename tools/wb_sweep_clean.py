#!/usr/bin/env python3
"""WB clean sweep: CONFIG_10 through CONFIG_16, no noise injection.
Measures throughput on VB-Cable loopback for each config."""
import subprocess, socket, time, threading, os, sys

# Force unbuffered output
sys.stdout.reconfigure(line_buffering=True)
sys.stderr.reconfigure(line_buffering=True)

MERCURY = r"C:\Program Files\Mercury\mercury.exe"
VB_IN = "CABLE Output (VB-Audio Virtual Cable)"
VB_OUT = "CABLE Input (VB-Audio Virtual Cable)"
RSP_PORT = 7002
CMD_PORT = 7006
MEASURE_DURATION = 60  # seconds per config
SETTLE_TIME = 10       # wait for connection before measuring

CONFIGS = list(range(10, 17))  # CONFIG_10 through CONFIG_16

def kill_mercury():
    subprocess.run(["cmd.exe", "/c", "taskkill /F /IM mercury.exe"],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

def tcp_connect(port, timeout=15):
    deadline = time.time() + timeout
    last_err = None
    while time.time() < deadline:
        s = socket.socket()
        s.settimeout(5)
        try:
            s.connect(('127.0.0.1', port))
            return s
        except (ConnectionRefusedError, OSError) as e:
            last_err = e
            try: s.close()
            except: pass
            time.sleep(0.5)
    raise ConnectionError(f"Cannot connect to port {port}: {last_err}")

def collect_output(proc, lines):
    try:
        for raw in proc.stdout:
            line = raw.decode(errors='replace').rstrip()
            lines.append(line)
    except:
        pass

def run_config(config_id):
    """Run a single config test, return (config, rx_bytes, duration, bps, bpm, ok, fail, gate)."""
    print(f"\n{'='*60}")
    print(f"CONFIG_{config_id}")
    print(f"{'='*60}")
    sys.stdout.flush()

    # Kill existing
    kill_mercury()
    time.sleep(2)

    rsp_lines, cmd_lines = [], []
    sockets = []

    try:
        # Start responder
        print(f"  Starting responder...", flush=True)
        rsp = subprocess.Popen([
            MERCURY, "-m", "ARQ", "-s", str(config_id), "-R",
            "-p", str(RSP_PORT), "-i", VB_IN, "-o", VB_OUT, "-x", "wasapi", "-n", "-Q", "0"
        ], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        threading.Thread(target=collect_output, args=(rsp, rsp_lines), daemon=True).start()
        time.sleep(4)

        # Start commander
        print(f"  Starting commander...", flush=True)
        cmd = subprocess.Popen([
            MERCURY, "-m", "ARQ", "-s", str(config_id), "-R",
            "-p", str(CMD_PORT), "-i", VB_IN, "-o", VB_OUT, "-x", "wasapi", "-n", "-Q", "0"
        ], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        threading.Thread(target=collect_output, args=(cmd, cmd_lines), daemon=True).start()
        time.sleep(3)

        # Setup responder (persistent TCP)
        print(f"  Connecting TCP...", flush=True)
        rsp_ctrl = tcp_connect(RSP_PORT)
        sockets.append(rsp_ctrl)
        rsp_ctrl.settimeout(5)
        rsp_ctrl.sendall(b"MYCALL TESTB\r")
        time.sleep(0.5)
        try: rsp_ctrl.recv(256)
        except: pass
        rsp_ctrl.sendall(b"LISTEN ON\r")
        time.sleep(0.5)
        try: rsp_ctrl.recv(256)
        except: pass

        # Data ports
        cmd_data = tcp_connect(CMD_PORT + 1)
        sockets.append(cmd_data)
        rsp_data = tcp_connect(RSP_PORT + 1)
        sockets.append(rsp_data)
        rsp_data.settimeout(2)

        # TX thread
        stop = threading.Event()
        rx_bytes = [0]

        def tx_loop():
            chunk = bytes(range(256)) * 4
            cmd_data.settimeout(30)
            while not stop.is_set():
                try:
                    cmd_data.send(chunk)
                except:
                    break
                time.sleep(0.05)

        def rx_loop():
            while not stop.is_set():
                try:
                    data = rsp_data.recv(4096)
                    if data:
                        rx_bytes[0] += len(data)
                except socket.timeout:
                    continue
                except:
                    break

        threading.Thread(target=tx_loop, daemon=True).start()
        threading.Thread(target=rx_loop, daemon=True).start()
        time.sleep(1)

        # Connect
        print(f"  Connecting ARQ...", flush=True)
        cmd_ctrl = tcp_connect(CMD_PORT)
        sockets.append(cmd_ctrl)
        cmd_ctrl.sendall(b"CONNECT TESTA TESTB\r")

        # Wait for data to flow (more reliable than checking stdout for CONNECTED)
        print(f"  Waiting for data flow...", flush=True)
        t0 = time.time()
        while time.time() - t0 < 40:
            if rx_bytes[0] > 0:
                break
            time.sleep(1)

        if rx_bytes[0] > 0:
            print(f"  Data flowing ({rx_bytes[0]} bytes after {time.time()-t0:.0f}s)", flush=True)
        else:
            print(f"  WARNING: No data after 40s", flush=True)

        # Settle
        time.sleep(SETTLE_TIME)

        # Measure
        start_rx = rx_bytes[0]
        start_time = time.time()
        time.sleep(MEASURE_DURATION)
        end_rx = rx_bytes[0]
        elapsed = time.time() - start_time

        delta = end_rx - start_rx
        bps = delta * 8 / elapsed if elapsed > 0 else 0
        bpm = delta * 60 / elapsed if elapsed > 0 else 0

        # Decode stats
        ok = sum(1 for l in rsp_lines if "RX-DECODE" in l and " OK" in l)
        fail = sum(1 for l in rsp_lines if "RX-DECODE" in l and "FAIL" in l)
        gate = sum(1 for l in rsp_lines if "ACK-GATE" in l)

        print(f"  RX: {delta} bytes in {elapsed:.1f}s", flush=True)
        print(f"  Throughput: {bps:.0f} bps ({bpm:.0f} B/min)", flush=True)
        print(f"  Decode: {ok} OK, {fail} FAIL, {gate} ACK-GATE", flush=True)

        # Verify WB mode
        nb_clamp = any("NB mode: clamping" in l for l in rsp_lines)
        if nb_clamp:
            print(f"  WARNING: Running in NB mode (clamped)!", flush=True)

        # Stop threads
        stop.set()
        time.sleep(1)

        return (config_id, delta, elapsed, bps, bpm, ok, fail, gate)

    finally:
        # Cleanup
        for s in sockets:
            try: s.close()
            except: pass
        try: rsp.kill()
        except: pass
        try: cmd.kill()
        except: pass
        kill_mercury()
        time.sleep(2)


def main():
    print(f"WB Clean Sweep: CONFIG_{CONFIGS[0]} to CONFIG_{CONFIGS[-1]}", flush=True)
    print(f"Duration: {MEASURE_DURATION}s per config, {SETTLE_TIME}s settle", flush=True)
    print(f"No noise injection (clean VB-Cable)", flush=True)

    results = []
    for cfg in CONFIGS:
        try:
            r = run_config(cfg)
            results.append(r)
        except Exception as e:
            print(f"  ERROR: {e}", flush=True)
            import traceback; traceback.print_exc()
            results.append((cfg, 0, 0, 0, 0, 0, 0, 0))
            kill_mercury()
            time.sleep(2)

    # Summary table
    print(f"\n{'='*60}", flush=True)
    print(f"SUMMARY — WB Clean Sweep", flush=True)
    print(f"{'='*60}", flush=True)
    print(f"{'Config':<12} {'B/min':>8} {'bps':>8} {'OK':>6} {'FAIL':>6} {'Gate':>6}", flush=True)
    print(f"{'-'*12} {'-'*8} {'-'*8} {'-'*6} {'-'*6} {'-'*6}", flush=True)
    for cfg, delta, elapsed, bps, bpm, ok, fail, gate in results:
        print(f"CONFIG_{cfg:<5} {bpm:>8.0f} {bps:>8.0f} {ok:>6} {fail:>6} {gate:>6}", flush=True)

    # Theoretical comparison
    CONFIG_MAX_BPS = {
        10: 2102, 11: 2523, 12: 2943, 13: 3363,
        14: 3924, 15: 4785, 16: 5735,
    }
    print(f"\n{'Config':<12} {'Actual':>8} {'PHY max':>8} {'Efficiency':>10}", flush=True)
    print(f"{'-'*12} {'-'*8} {'-'*8} {'-'*10}", flush=True)
    for cfg, delta, elapsed, bps, bpm, ok, fail, gate in results:
        phy = CONFIG_MAX_BPS.get(cfg, 0)
        eff = (bps / phy * 100) if phy > 0 else 0
        print(f"CONFIG_{cfg:<5} {bps:>7.0f}  {phy:>7}  {eff:>8.1f}%", flush=True)

if __name__ == "__main__":
    main()
