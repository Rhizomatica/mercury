#!/usr/bin/env python3
"""
ROBUST mode ARQ loopback test for Mercury modem.
Starts commander + responder on VB-Cable, connects via TCP, monitors for 120s.
Usage: python robust_loopback_test.py [100|101|102] [timeout_seconds] [--gearshift]
  100 = ROBUST_0 (32-MFSK, 1 stream, LDPC 1/16)
  101 = ROBUST_1 (16-MFSK, 2 streams, LDPC 1/16)
  102 = ROBUST_2 (16-MFSK, 2 streams, LDPC 1/4)
  --gearshift: enable SUCCESS_BASED_LADDER gearshift (-g flag)
"""
import subprocess, socket, time, sys, threading, os, signal

MERCURY = r"C:\Program Files\Mercury\mercury.exe"
VB_OUT = "CABLE Input (VB-Audio Virtual Cable)"
VB_IN  = "CABLE Output (VB-Audio Virtual Cable)"

config = int(sys.argv[1]) if len(sys.argv) > 1 and not sys.argv[1].startswith('-') else 100
timeout_s = int(sys.argv[2]) if len(sys.argv) > 2 and not sys.argv[2].startswith('-') else 120
gearshift = '--gearshift' in sys.argv or '-g' in sys.argv
narrowband = '--nb' in sys.argv or '-N' in sys.argv
no_robust = '--no-robust' in sys.argv  # skip -R flag (for testing OFDM configs directly)
verbose = '--verbose' in sys.argv or '-v' in sys.argv

RSP_PORT = 7002  # responder control=7002, data=7003
CMD_PORT = 7006  # commander control=7006, data=7007

def log_output(proc, label, logfile, start_time):
    """Read stdout from process and write to logfile and console with timestamps."""
    for line in iter(proc.stdout.readline, b''):
        text = line.decode('utf-8', errors='replace').rstrip()
        elapsed = time.time() - start_time
        entry = f"[T+{elapsed:010.3f}] [{label}] {text}\n"
        sys.stdout.write(entry)
        sys.stdout.flush()
        logfile.write(entry)
        logfile.flush()

def tcp_send(port, commands, label="", retries=10, delay=1):
    """Connect to TCP port and send list of commands, return socket."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(5)
    for attempt in range(retries):
        try:
            sock.connect(("127.0.0.1", port))
            break
        except ConnectionRefusedError:
            if attempt == retries - 1:
                raise
            print(f"[{label}] Port {port} not ready, retrying ({attempt+1}/{retries})...")
            time.sleep(delay)
            sock.close()
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sock.settimeout(5)
    print(f"[{label}] Connected to port {port}")
    for cmd in commands:
        print(f"[{label}] Sending: {cmd.strip()}")
        sock.sendall(cmd.encode())
        time.sleep(0.3)
        # Read any immediate response
        try:
            sock.settimeout(0.5)
            data = sock.recv(4096)
            if data:
                print(f"[{label}] Response: {data.decode('utf-8', errors='replace').strip()}")
        except socket.timeout:
            pass
    return sock

def tx_thread_fn(sock, stop_event):
    """Push data to commander data port for gearshift testing."""
    chunk = bytes(range(256)) * 4  # 1024 bytes
    sock.settimeout(30)
    while not stop_event.is_set():
        try:
            sock.send(chunk)
        except socket.timeout:
            continue
        except (ConnectionError, OSError):
            break
        time.sleep(0.05)

def rx_thread_fn(sock, stop_event, logfile):
    """Read data from responder data port."""
    total = 0
    sock.settimeout(2)
    while not stop_event.is_set():
        try:
            data = sock.recv(4096)
            if not data:
                break
            total += len(data)
        except socket.timeout:
            continue
        except (ConnectionError, OSError):
            break

def main():
    mode_name = {100: "ROBUST_0", 101: "ROBUST_1", 102: "ROBUST_2"}.get(config, f"CONFIG_{config}")
    bw = "NB" if narrowband else "WB"
    print(f"=== Mercury ROBUST Loopback Test: {bw} {mode_name} (config={config}) ===")
    print(f"Timeout: {timeout_s}s, Gearshift: {'ON' if gearshift else 'OFF'}, Narrowband: {'ON' if narrowband else 'OFF'}")
    print(f"Commander ports: ctrl={CMD_PORT} data={CMD_PORT+1}")
    print(f"Responder ports: ctrl={RSP_PORT} data={RSP_PORT+1}")
    print()

    # Kill any existing mercury processes
    os.system("taskkill /F /IM mercury.exe 2>nul")
    time.sleep(1)

    logpath = f"robust_{bw.lower()}_{config}_test.log"
    logfile = open(logpath, "w")
    procs = []
    sockets = []
    stop_event = threading.Event()

    # T=0 reference for all timestamps (set before processes start)
    t0 = time.time()

    try:
        # Start responder (headless)
        rsp_cmd = [
            MERCURY, "-m", "ARQ", "-s", str(config)] + ([] if no_robust else ["-R"]) + [
            "-p", str(RSP_PORT),
            "-i", VB_IN, "-o", VB_OUT,
            "-x", "wasapi", "-n"
        ] + (["-g"] if gearshift else []) + (["-N"] if narrowband else ["-W"]) + (["-v"] if verbose else [])
        print(f"Starting responder: {' '.join(rsp_cmd)}")
        rsp = subprocess.Popen(rsp_cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        procs.append(rsp)
        t_rsp = threading.Thread(target=log_output, args=(rsp, "RSP", logfile, t0), daemon=True)
        t_rsp.start()
        time.sleep(3)

        # Start commander (headless)
        cmd_cmd = [
            MERCURY, "-m", "ARQ", "-s", str(config)] + ([] if no_robust else ["-R"]) + [
            "-p", str(CMD_PORT),
            "-i", VB_IN, "-o", VB_OUT,
            "-x", "wasapi", "-n"
        ] + (["-g"] if gearshift else []) + (["-N"] if narrowband else ["-W"]) + (["-v"] if verbose else [])
        print(f"Starting commander: {' '.join(cmd_cmd)}")
        cmd = subprocess.Popen(cmd_cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        procs.append(cmd)
        t_cmd = threading.Thread(target=log_output, args=(cmd, "CMD", logfile, t0), daemon=True)
        t_cmd.start()
        time.sleep(3)

        # Set up responder: set callsign and start listening
        print("\n--- Setting up responder ---")
        rsp_sock = tcp_send(RSP_PORT, [
            "MYCALL TESTB\r\n",
            "LISTEN ON\r\n",
        ], "RSP-TCP")
        sockets.append(rsp_sock)
        time.sleep(1)

        # Connect data ports for gearshift test (need data flowing for BLOCK_END)
        if gearshift:
            print("\n--- Connecting data ports for gearshift test ---")
            cmd_data = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            cmd_data.settimeout(5)
            cmd_data.connect(("127.0.0.1", CMD_PORT + 1))
            sockets.append(cmd_data)

            rsp_data = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            rsp_data.settimeout(5)
            rsp_data.connect(("127.0.0.1", RSP_PORT + 1))
            sockets.append(rsp_data)

            # Start TX thread to fill commander FIFO
            tx_t = threading.Thread(target=tx_thread_fn, args=(cmd_data, stop_event), daemon=True)
            tx_t.start()
            print("Pre-filling commander FIFO...")
            time.sleep(2)

            # Start RX thread to drain responder data
            rx_t = threading.Thread(target=rx_thread_fn, args=(rsp_data, stop_event, logfile), daemon=True)
            rx_t.start()

        # Set up commander: send CONNECT
        print("\n--- Setting up commander ---")
        cmd_sock = tcp_send(CMD_PORT, [
            "CONNECT TESTA TESTB\r\n",
        ], "CMD-TCP")
        sockets.append(cmd_sock)

        # Monitor TCP responses from commander
        cmd_sock.settimeout(2)
        rsp_sock.settimeout(0.1)
        start = time.time()
        print(f"\n=== Monitoring for {timeout_s}s ===\n")
        while time.time() - start < timeout_s:
            # Check commander TCP
            try:
                data = cmd_sock.recv(4096)
                if data:
                    text = data.decode('utf-8', errors='replace').strip()
                    elapsed = time.time() - t0
                    msg = f"[T+{elapsed:010.3f}] [CMD-TCP] {text}"
                    print(msg)
                    logfile.write(msg + "\n")
                    logfile.flush()
            except socket.timeout:
                pass
            except ConnectionError:
                print("[CMD-TCP] Connection lost")
                break

            # Check responder TCP
            try:
                data = rsp_sock.recv(4096)
                if data:
                    text = data.decode('utf-8', errors='replace').strip()
                    elapsed = time.time() - t0
                    msg = f"[T+{elapsed:010.3f}] [RSP-TCP] {text}"
                    print(msg)
                    logfile.write(msg + "\n")
                    logfile.flush()
            except socket.timeout:
                pass
            except ConnectionError:
                pass

            # Check if processes are still alive
            for i, p in enumerate(procs):
                if p.poll() is not None:
                    elapsed = time.time() - t0
                    label = "RSP" if i == 0 else "CMD"
                    print(f"[WARN] {label} process exited with code {p.returncode} at T+{elapsed:.1f}s")

        elapsed = time.time() - start
        print(f"\n=== Test complete after {elapsed:.1f}s ===")

    except Exception as e:
        import traceback
        print(f"ERROR: {e}")
        traceback.print_exc()
    finally:
        # Cleanup
        stop_event.set()
        print("Killing mercury processes...")
        for s in sockets:
            try: s.close()
            except: pass
        for p in procs:
            try: p.kill()
            except: pass
        os.system("taskkill /F /IM mercury.exe 2>nul")
        logfile.close()
        print(f"Log saved to: {logpath}")

if __name__ == "__main__":
    main()
