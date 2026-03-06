#!/usr/bin/env python3
"""
Mercury modem ARQ throughput test.
Starts commander + responder on VB-Cable, connects, pushes data, measures throughput.
Usage: python throughput_test.py [config] [duration_seconds]
  100 = ROBUST_0, 101 = ROBUST_1, 102 = ROBUST_2, 0 = CONFIG_0
"""
import subprocess, socket, time, sys, threading, os

MERCURY = r"C:\Program Files\Mercury\mercury.exe"
VB_OUT = "CABLE Input (VB-Audio Virtual Cable)"
VB_IN  = "CABLE Output (VB-Audio Virtual Cable)"

config = int(sys.argv[1]) if len(sys.argv) > 1 else 0
duration_s = int(sys.argv[2]) if len(sys.argv) > 2 else 180
# Extra flags: --wb for wideband auto, --gain N for RX gain boost
extra_flags = []
gain_flag = []
text_file = None
i = 3
while i < len(sys.argv):
    if sys.argv[i] == "--wb":
        extra_flags += ["-M", "auto", "-g"]
    elif sys.argv[i] == "--gain" and i + 1 < len(sys.argv):
        gain_flag = ["-G", sys.argv[i+1]]
        i += 1
    elif sys.argv[i] == "--text" and i + 1 < len(sys.argv):
        text_file = sys.argv[i+1]
        i += 1
    i += 1

RSP_PORT = 7002
CMD_PORT = 7006

robust_flag = ["-R"] if config >= 100 else []

rx_bytes = 0
rx_lock = threading.Lock()
rx_events = []  # (timestamp, delta_bytes) for each recv
stop_flag = False

def capture_output(proc, label, lines):
    for line in iter(proc.stdout.readline, b''):
        text = line.decode('utf-8', errors='replace').rstrip()
        lines.append(text)

def rx_thread_fn(sock):
    global rx_bytes, stop_flag
    sock.settimeout(2)
    while not stop_flag:
        try:
            data = sock.recv(4096)
            if not data:
                break
            with rx_lock:
                rx_bytes += len(data)
                rx_events.append((time.time(), len(data), rx_bytes))
                print(f"  [RX] +{len(data)}B (total: {rx_bytes}B)")
        except socket.timeout:
            continue
        except (ConnectionError, OSError):
            break

def tx_thread_fn(sock, start_event):
    """Push data continuously until stop_flag."""
    global stop_flag, text_file
    if text_file:
        with open(text_file, 'rb') as f:
            text_data = f.read()
        print(f"  TX source: text file ({len(text_data)} bytes)")
    else:
        text_data = None
    chunk = text_data if text_data else bytes(range(256)) * 4  # 1024 bytes
    start_event.wait()  # Wait for signal to start
    sock.settimeout(30)
    offset = 0
    while not stop_flag:
        try:
            if text_data:
                end = min(offset + 4096, len(chunk))
                sock.send(chunk[offset:end])
                offset = end
                if offset >= len(chunk):
                    offset = 0  # loop
            else:
                sock.send(chunk)
        except socket.timeout:
            continue
        except (ConnectionError, OSError):
            break
        time.sleep(0.02)

def wait_for_connected(sock, timeout=120):
    sock.settimeout(2)
    start = time.time()
    while time.time() - start < timeout:
        try:
            data = sock.recv(4096)
            if data:
                text = data.decode('utf-8', errors='replace')
                if 'CONNECTED' in text:
                    return True
        except socket.timeout:
            continue
        except ConnectionError:
            return False
    return False

def main():
    global stop_flag
    mode_name = {100: "ROBUST_0", 101: "ROBUST_1", 102: "ROBUST_2"}.get(config, f"CONFIG_{config}")
    print(f"=== Mercury Throughput Test: {mode_name} (config={config}) ===")
    print(f"Duration: {duration_s}s after connection")
    print()

    os.system("taskkill /F /IM mercury.exe 2>nul")
    time.sleep(1)

    procs = []
    sockets = []
    cmd_lines = []
    rsp_lines = []

    try:
        # Start responder
        rsp_cmd = [MERCURY, "-m", "ARQ", "-s", str(config)] + robust_flag + extra_flags + gain_flag + [
            "-p", str(RSP_PORT), "-i", VB_IN, "-o", VB_OUT, "-x", "wasapi", "-n"
        ]
        print("Starting responder...")
        rsp = subprocess.Popen(rsp_cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        procs.append(rsp)
        threading.Thread(target=capture_output, args=(rsp, "RSP", rsp_lines), daemon=True).start()
        time.sleep(3)

        # Start commander
        cmd_cmd = [MERCURY, "-m", "ARQ", "-s", str(config)] + robust_flag + extra_flags + gain_flag + [
            "-p", str(CMD_PORT), "-i", VB_IN, "-o", VB_OUT, "-x", "wasapi", "-n"
        ]
        print("Starting commander...")
        cmd = subprocess.Popen(cmd_cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        procs.append(cmd)
        threading.Thread(target=capture_output, args=(cmd, "CMD", cmd_lines), daemon=True).start()
        time.sleep(3)

        # Connect data ports EARLY (before ARQ connection) to pre-fill FIFO
        rsp_data = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        rsp_data.settimeout(5)
        rsp_data.connect(("127.0.0.1", RSP_PORT + 1))
        sockets.append(rsp_data)

        cmd_data = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        cmd_data.settimeout(5)
        cmd_data.connect(("127.0.0.1", CMD_PORT + 1))
        sockets.append(cmd_data)

        # Start TX thread immediately to fill FIFO before connection
        tx_start = threading.Event()
        tx_t = threading.Thread(target=tx_thread_fn, args=(cmd_data, tx_start), daemon=True)
        tx_t.start()
        tx_start.set()  # Start pushing data now
        print("Pre-filling commander FIFO with data...")
        time.sleep(2)

        # Start RX thread
        rx_t = threading.Thread(target=rx_thread_fn, args=(rsp_data,), daemon=True)
        rx_t.start()

        # Set up responder
        rsp_ctrl = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        rsp_ctrl.settimeout(5)
        rsp_ctrl.connect(("127.0.0.1", RSP_PORT))
        sockets.append(rsp_ctrl)
        rsp_ctrl.sendall(b"MYCALL TESTB\r\n")
        time.sleep(0.3)
        rsp_ctrl.sendall(b"LISTEN ON\r\n")
        time.sleep(1)

        # Commander: initiate connection
        cmd_ctrl = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        cmd_ctrl.settimeout(5)
        cmd_ctrl.connect(("127.0.0.1", CMD_PORT))
        sockets.append(cmd_ctrl)

        t0 = time.time()
        cmd_ctrl.sendall(b"CONNECT TESTA TESTB\r\n")
        print("Sent CONNECT, waiting for link...")

        if not wait_for_connected(cmd_ctrl, timeout=120):
            print("ERROR: Connection not established within timeout")
            return

        connect_time = time.time()
        print(f"CONNECTED after {connect_time - t0:.1f}s")
        print(f"Running data transfer for {duration_s}s...\n")

        # Monitor for duration_s
        last_status = connect_time
        while time.time() - connect_time < duration_s:
            time.sleep(5)
            now = time.time()
            if now - last_status > 30:
                with rx_lock:
                    r = rx_bytes
                    ne = len(rx_events)
                elapsed = now - connect_time
                bps = r * 8 / elapsed if elapsed > 0 else 0
                print(f"  [{elapsed:.0f}s] RX={r}B  events={ne}  avg={bps:.1f}bps")
                last_status = now

            # Check processes alive
            for p in procs:
                if p.poll() is not None:
                    print(f"  WARN: Process exited with code {p.returncode}")

        # Stop TX, wait for last data
        stop_flag = True
        print(f"\nStopping after {duration_s}s. Waiting 30s for final data...")
        prev_rx = 0
        for _ in range(6):
            time.sleep(5)
            with rx_lock:
                cur = rx_bytes
            if cur > prev_rx:
                print(f"  Still receiving... ({cur}B)")
                prev_rx = cur
            elif cur > 0:
                break

        # Results
        with rx_lock:
            total_rx = rx_bytes
            events = list(rx_events)

        total_elapsed = time.time() - connect_time

        print(f"\n{'='*60}")
        print(f"Mode: {mode_name} (config={config})")
        print(f"Handshake: {connect_time - t0:.1f}s")
        print(f"RX total: {total_rx} bytes in {total_elapsed:.1f}s")

        if len(events) >= 2:
            first_t = events[0][0]
            last_t = events[-1][0]
            window = last_t - first_t
            if window > 1:
                eff = total_rx * 8 / window
                print(f"Throughput (first-to-last RX): {eff:.1f} bps over {window:.1f}s")

            avg = total_rx * 8 / total_elapsed
            print(f"Average throughput (incl. startup): {avg:.1f} bps over {total_elapsed:.1f}s")

            # Block analysis
            blocks = []
            cur_block_start = events[0][0]
            cur_block_bytes = events[0][1]
            for i in range(1, len(events)):
                gap = events[i][0] - events[i-1][0]
                if gap > 2.0:  # Gap > 2s = new block
                    blocks.append((cur_block_start, cur_block_bytes))
                    cur_block_start = events[i][0]
                    cur_block_bytes = events[i][1]
                else:
                    cur_block_bytes += events[i][1]
            blocks.append((cur_block_start, cur_block_bytes))

            print(f"\nData blocks received: {len(blocks)}")
            for i, (t, b) in enumerate(blocks):
                dt = t - blocks[i-1][0] if i > 0 else 0
                bps = b * 8 / dt if dt > 0 else 0
                print(f"  Block {i}: {b}B  dt={dt:.1f}s" + (f"  ({bps:.1f} bps)" if dt > 0 else ""))
        elif len(events) == 1:
            print(f"Only 1 RX event ({events[0][1]}B) — need longer test")
        else:
            print("No data received!")

        # Final stats from modem stdout
        print("\nCommander stats (last output):")
        for line in cmd_lines[-40:]:
            if any(k in line for k in ['nSent_data', 'nAcked_data', 'nReSent', 'nReceived_data', 'nLost']):
                print(f"  {line.strip()}")

        # Debug output
        print("\nCommander debug lines:")
        for line in cmd_lines:
            if any(k in line for k in ['DBG-', 'RX-DATA', 'RX-TIMING', 'BUFFER', 'connection_status', 'CMD-ACK', 'ACK-RX', 'TX-ACK']):
                print(f"  {line.strip()}")
        print("\nResponder debug lines:")
        for line in rsp_lines:
            if any(k in line for k in ['DBG-', 'RX-DATA', 'RX-TIMING', 'BUFFER', 'connection_status', 'TX-ACK', 'ACK-RX']):
                print(f"  {line.strip()}")

        print(f"{'='*60}")

    except Exception as e:
        import traceback
        print(f"ERROR: {e}")
        traceback.print_exc()
    finally:
        stop_flag = True
        print("\nCleaning up...")
        for s in sockets:
            try: s.close()
            except: pass
        for p in procs:
            try: p.kill()
            except: pass
        os.system("taskkill /F /IM mercury.exe 2>nul")

if __name__ == "__main__":
    main()
