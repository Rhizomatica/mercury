#!/usr/bin/env python3
"""
ROBUST mode ARQ loopback test for Mercury modem.
Starts commander + responder on VB-Cable, connects via TCP, monitors for 120s.
Usage: python robust_loopback_test.py [100|101|102] [timeout_seconds]
  100 = ROBUST_0 (32-MFSK, 1 stream, LDPC 1/16)
  101 = ROBUST_1 (16-MFSK, 2 streams, LDPC 1/16)
  102 = ROBUST_2 (16-MFSK, 2 streams, LDPC 1/4)
"""
import subprocess, socket, time, sys, threading, os, signal

MERCURY = r"C:\Program Files\Mercury\mercury.exe"
VB_OUT = "CABLE Input (VB-Audio Virtual Cable)"
VB_IN  = "CABLE Output (VB-Audio Virtual Cable)"

config = int(sys.argv[1]) if len(sys.argv) > 1 else 100
timeout_s = int(sys.argv[2]) if len(sys.argv) > 2 else 120

RSP_PORT = 7001  # responder control=7001, data=7002
CMD_PORT = 7005  # commander control=7005, data=7006

def log_output(proc, label, logfile):
    """Read stdout from process and write to logfile and console."""
    for line in iter(proc.stdout.readline, b''):
        text = line.decode('utf-8', errors='replace').rstrip()
        entry = f"[{label}] {text}\n"
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

def main():
    mode_name = {100: "ROBUST_0", 101: "ROBUST_1", 102: "ROBUST_2"}.get(config, f"CONFIG_{config}")
    print(f"=== Mercury ROBUST Loopback Test: {mode_name} (config={config}) ===")
    print(f"Timeout: {timeout_s}s")
    print(f"Commander ports: ctrl={CMD_PORT} data={CMD_PORT+1}")
    print(f"Responder ports: ctrl={RSP_PORT} data={RSP_PORT+1}")
    print()

    # Kill any existing mercury processes
    os.system("taskkill /F /IM mercury.exe 2>nul")
    time.sleep(1)

    logpath = f"robust_{config}_test.log"
    logfile = open(logpath, "w")
    procs = []
    sockets = []

    try:
        # Start responder (headless)
        rsp_cmd = [
            MERCURY, "-m", "ARQ", "-s", str(config), "-R",
            "-p", str(RSP_PORT),
            "-i", VB_IN, "-o", VB_OUT,
            "-x", "wasapi", "-n"
        ]
        print(f"Starting responder: {' '.join(rsp_cmd)}")
        rsp = subprocess.Popen(rsp_cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        procs.append(rsp)
        t_rsp = threading.Thread(target=log_output, args=(rsp, "RSP", logfile), daemon=True)
        t_rsp.start()
        time.sleep(3)

        # Start commander (headless)
        cmd_cmd = [
            MERCURY, "-m", "ARQ", "-s", str(config), "-R",
            "-p", str(CMD_PORT),
            "-i", VB_IN, "-o", VB_OUT,
            "-x", "wasapi", "-n"
        ]
        print(f"Starting commander: {' '.join(cmd_cmd)}")
        cmd = subprocess.Popen(cmd_cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        procs.append(cmd)
        t_cmd = threading.Thread(target=log_output, args=(cmd, "CMD", logfile), daemon=True)
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
                    elapsed = time.time() - start
                    msg = f"[CMD-TCP @ {elapsed:.1f}s] {text}"
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
                    elapsed = time.time() - start
                    msg = f"[RSP-TCP @ {elapsed:.1f}s] {text}"
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
                    elapsed = time.time() - start
                    label = "RSP" if i == 0 else "CMD"
                    print(f"[WARN] {label} process exited with code {p.returncode} at {elapsed:.1f}s")

        elapsed = time.time() - start
        print(f"\n=== Test complete after {elapsed:.1f}s ===")

    except Exception as e:
        import traceback
        print(f"ERROR: {e}")
        traceback.print_exc()
    finally:
        # Cleanup
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
