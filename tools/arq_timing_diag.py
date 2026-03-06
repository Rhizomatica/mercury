#!/usr/bin/env python3
"""Quick ARQ timing diagnostic: runs CONFIG_5 in WB and NB, captures output."""
import subprocess, threading, time, sys, socket, os

MERCURY = r"C:\Program Files\Mercury\mercury.exe"
VB_IN = "CABLE Output (VB-Audio Virtual Cable)"
VB_OUT = "CABLE Input (VB-Audio Virtual Cable)"

def collect_output(proc, lines, label):
    for raw in iter(proc.stdout.readline, b''):
        line = raw.decode('utf-8', errors='replace').rstrip()
        lines.append(line)

def tcp_connect(port, retries=5):
    for i in range(retries):
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            s.settimeout(5)
            s.connect(('127.0.0.1', port))
            return s
        except:
            time.sleep(1)
    return None

def run_test(config, is_nb, duration_s=90):
    label = f"CONFIG_{config}_{'NB' if is_nb else 'WB'}"
    print(f"\n{'='*70}")
    print(f"Testing {label} for {duration_s}s")
    print(f"{'='*70}\n")

    os.system("taskkill /F /IM mercury.exe 2>nul >nul")
    time.sleep(2)

    base_args = [MERCURY, "-m", "ARQ", "-s", str(config), "-Q", "0",
                 "-i", VB_IN, "-o", VB_OUT, "-x", "wasapi", "-n"]
    if is_nb:
        base_args.append("-N")
    else:
        base_args.append("-W")

    rsp_lines, cmd_lines = [], []

    # Start responder (port 7002)
    rsp = subprocess.Popen(base_args + ["-p", "7002"],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    threading.Thread(target=collect_output, args=(rsp, rsp_lines, "RSP"), daemon=True).start()
    time.sleep(4)

    # Start commander (port 7006)
    cmd = subprocess.Popen(base_args + ["-p", "7006"],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    threading.Thread(target=collect_output, args=(cmd, cmd_lines, "CMD"), daemon=True).start()
    time.sleep(3)

    # Setup responder
    rsp_ctrl = tcp_connect(7002)
    rsp_ctrl.send(b"MYCALL TESTB\r\n")
    time.sleep(0.5)
    rsp_ctrl.send(b"LISTEN ON\r\n")
    time.sleep(1)

    # Data ports
    cmd_data = tcp_connect(7007)
    rsp_data = tcp_connect(7003)

    # TX thread
    stop = threading.Event()
    def tx_loop():
        chunk = bytes(range(256)) * 4
        cmd_data.settimeout(30)
        while not stop.is_set():
            try:
                cmd_data.send(chunk)
            except:
                break
            time.sleep(0.05)
    tx_thread = threading.Thread(target=tx_loop, daemon=True)
    tx_thread.start()
    time.sleep(2)

    # RX thread
    rx_bytes = [0]
    rx_events = []
    def rx_loop():
        rsp_data.settimeout(2)
        while not stop.is_set():
            try:
                data = rsp_data.recv(4096)
                if not data:
                    break
                rx_bytes[0] += len(data)
                rx_events.append((time.time(), len(data), rx_bytes[0]))
            except socket.timeout:
                continue
            except:
                break
    rx_thread = threading.Thread(target=rx_loop, daemon=True)
    rx_thread.start()

    # Connect
    cmd_ctrl = tcp_connect(7006)
    cmd_ctrl.send(b"CONNECT TESTA TESTB\r\n")

    # Wait for connection
    cmd_ctrl.settimeout(2)
    buf = b''
    connected = False
    start = time.time()
    while time.time() - start < 120:
        try:
            data = cmd_ctrl.recv(4096)
            if data:
                buf += data
                if b'CONNECTED' in buf and b'DISCONNECTED' not in buf:
                    connected = True
                    break
        except socket.timeout:
            continue

    if not connected:
        print(f"FAILED TO CONNECT {label}")
        print(f"\n--- CMD LINES ({len(cmd_lines)}) ---")
        for l in cmd_lines[-30:]:
            print(f"  {l}")
        print(f"\n--- RSP LINES ({len(rsp_lines)}) ---")
        for l in rsp_lines[-30:]:
            print(f"  {l}")
        stop.set()
        rsp.kill(); cmd.kill()
        time.sleep(2)
        return

    connect_time = time.time() - start
    print(f"Connected in {connect_time:.1f}s")

    # Wait a moment for data to start flowing
    time.sleep(5)

    # Measure
    start_bytes = rx_bytes[0]
    measure_start = time.time()

    # Print progress every 15s
    while time.time() - measure_start < duration_s:
        time.sleep(15)
        elapsed = time.time() - measure_start
        cur_bytes = rx_bytes[0] - start_bytes
        bpm = cur_bytes * 60 / elapsed if elapsed > 0 else 0
        print(f"  [{elapsed:.0f}s] {cur_bytes}B received, {bpm:.0f} B/min")

    total_bytes = rx_bytes[0] - start_bytes
    total_time = time.time() - measure_start
    bpm = total_bytes * 60 / total_time
    bps = total_bytes * 8 / total_time

    print(f"\n--- {label} RESULTS ---")
    print(f"RX bytes: {total_bytes}")
    print(f"Duration: {total_time:.1f}s")
    print(f"Throughput: {bpm:.0f} B/min ({bps:.1f} bps)")

    # Parse key timing info from Mercury output
    print(f"\n--- KEY MERCURY OUTPUT ---")

    # Batch scaling info
    for lines, role in [(cmd_lines, "CMD"), (rsp_lines, "RSP")]:
        for l in lines:
            if "Batch scaling" in l:
                print(f"  [{role}] {l}")
            elif "ack_timeout" in l.lower() and "set" in l.lower():
                print(f"  [{role}] {l}")

    # Count key events
    for lines, role in [(cmd_lines, "CMD"), (rsp_lines, "RSP")]:
        batches = sum(1 for l in lines if "send_batch" in l)
        acks = sum(1 for l in lines if "ACK-DET" in l or "Ack received" in l or "ACK-RX" in l)
        nacks = sum(1 for l in lines if "NAck" in l and "nNAck" not in l)
        copies = sum(1 for l in lines if "DBG-COPY" in l)
        block_ends = sum(1 for l in lines if "BLOCK_END" in l or "end of block" in l)
        print(f"  [{role}] send_batch={batches}, ACKs={acks}, NAcks={nacks}, "
              f"DBG-COPY={copies}, BLOCK_END={block_ends}")

    # RX events timeline
    if rx_events:
        print(f"\n--- RX EVENTS (first 20, last 20) ---")
        events_to_show = rx_events[:20]
        if len(rx_events) > 40:
            events_to_show += rx_events[-20:]
        elif len(rx_events) > 20:
            events_to_show = rx_events
        t0 = rx_events[0][0]
        for t, sz, total in events_to_show:
            print(f"  t={t-t0:7.2f}s  +{sz:4d}B  total={total}B")

    # Print last 50 CMD lines for timing analysis
    print(f"\n--- LAST 50 CMD LINES ---")
    for l in cmd_lines[-50:]:
        print(f"  {l}")

    print(f"\n--- LAST 50 RSP LINES ---")
    for l in rsp_lines[-50:]:
        print(f"  {l}")

    stop.set()
    try: rsp_ctrl.close()
    except: pass
    try: cmd_ctrl.close()
    except: pass
    try: cmd_data.close()
    except: pass
    try: rsp_data.close()
    except: pass
    rsp.kill(); cmd.kill()
    time.sleep(2)

    return {
        'label': label,
        'rx_bytes': total_bytes,
        'bpm': bpm,
        'bps': bps,
        'connect_time': connect_time,
    }


if __name__ == '__main__':
    import sys
    configs = [(5, False), (5, True)]  # default: CONFIG_5 WB then NB
    dur = 90
    if len(sys.argv) > 1:
        configs = []
        for arg in sys.argv[1:]:
            if arg.startswith('--dur='):
                dur = int(arg.split('=')[1])
                continue
            parts = arg.split(':')
            cfg = int(parts[0])
            nb = parts[1].upper() == 'NB' if len(parts) > 1 else False
            configs.append((cfg, nb))

    results = []
    for cfg, nb in configs:
        r = run_test(cfg, is_nb=nb, duration_s=dur)
        if r:
            results.append(r)

    print(f"\n{'='*70}")
    print(f"COMPARISON")
    print(f"{'='*70}")
    for r in results:
        print(f"  {r['label']}: {r['bpm']:.0f} B/min ({r['bps']:.1f} bps)")
    if len(results) >= 2 and results[1]['bpm'] > 0:
        print(f"  Ratio: {results[0]['bpm']/results[1]['bpm']:.2f}x")
