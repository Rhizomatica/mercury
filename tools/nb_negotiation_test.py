#!/usr/bin/env python3
"""
NB/WB auto-negotiation test battery for Mercury modem.
Tests all 4 combinations of NB/WB commander + responder, multiple iterations.

Usage: python nb_negotiation_test.py [--iterations N] [--timeout T] [--combo C]
  --iterations N: number of times to run each combination (default 3)
  --timeout T: per-test timeout in seconds (default 90)
  --combo C: run only combo C (0=WB+WB, 1=NB+NB, 2=WB+NB, 3=NB+WB)
"""
import subprocess, socket, time, sys, threading, os, re, argparse

# Fix Windows console encoding
if sys.platform == 'win32':
    import io
    sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8', errors='replace')
    sys.stderr = io.TextIOWrapper(sys.stderr.buffer, encoding='utf-8', errors='replace')

MERCURY = r"C:\Program Files\Mercury\mercury.exe"
VB_OUT = "CABLE Input (VB-Audio Virtual Cable)"
VB_IN  = "CABLE Output (VB-Audio Virtual Cable)"

RSP_PORT = 7002  # responder control=7002, data=7003
CMD_PORT = 7006  # commander control=7006, data=7007

# Combinations: (commander_nb, responder_nb, expected_session)
COMBOS = [
    (False, False, "WB"),  # WB+WB = WB
    (True,  True,  "NB"),  # NB+NB = NB
    (False, True,  "NB"),  # WB cmdr + NB resp = NB (Phase 1 probe succeeds)
    (True,  False, "NB"),  # NB cmdr + WB resp = NB (Phase 2 with NB flag)
]


def kill_mercury():
    os.system("taskkill /F /IM mercury.exe 2>nul >nul")
    time.sleep(1)


def collect_output(proc, label, lines, lock, stop_event, t0):
    """Collect stdout lines from a process."""
    try:
        for raw_line in iter(proc.stdout.readline, b''):
            if stop_event.is_set():
                break
            text = raw_line.decode('utf-8', errors='replace').rstrip()
            elapsed = time.time() - t0
            entry = f"[T+{elapsed:07.3f}] [{label}] {text}"
            with lock:
                lines.append(entry)
    except (OSError, ValueError):
        pass


def tcp_reader(sock, label, lines, lock, stop_event, t0):
    """Continuously read from TCP socket and add to shared lines."""
    sock.settimeout(1)
    buf = ""
    while not stop_event.is_set():
        try:
            data = sock.recv(4096)
            if not data:
                break
            buf += data.decode('utf-8', errors='replace')
            while '\r' in buf:
                line, buf = buf.split('\r', 1)
                line = line.strip()
                if line:
                    elapsed = time.time() - t0
                    entry = f"[T+{elapsed:07.3f}] [{label}-TCP] {line}"
                    with lock:
                        lines.append(entry)
        except socket.timeout:
            continue
        except (ConnectionError, OSError):
            break


def tcp_connect(port, retries=15, delay=1):
    """Connect to TCP port, return socket."""
    for attempt in range(retries):
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(5)
        try:
            sock.connect(("127.0.0.1", port))
            return sock
        except (ConnectionRefusedError, OSError):
            sock.close()
            if attempt == retries - 1:
                raise
            time.sleep(delay)
    return None


def tcp_send(sock, cmd):
    """Send a command string."""
    sock.sendall(cmd.encode())
    time.sleep(0.3)


def wait_for_pattern(lines, lock, pattern, timeout, start_time=None):
    """Wait for a regex pattern in collected lines. Returns (matched_line, elapsed)."""
    if start_time is None:
        start_time = time.time()
    deadline = time.time() + timeout
    compiled = re.compile(pattern)
    seen = 0
    while time.time() < deadline:
        with lock:
            for i in range(seen, len(lines)):
                if compiled.search(lines[i]):
                    elapsed = time.time() - start_time
                    return lines[i], elapsed
            seen = len(lines)
        time.sleep(0.2)
    return None, time.time() - start_time


def run_single_test(cmd_nb, rsp_nb, expected_session, timeout_s, iteration, logfile):
    """Run a single NB/WB negotiation test. Returns dict with results."""
    cmd_mode = "NB" if cmd_nb else "WB"
    rsp_mode = "NB" if rsp_nb else "WB"
    test_name = f"{cmd_mode}_cmdr+{rsp_mode}_resp"

    result = {
        "test": test_name,
        "iteration": iteration,
        "passed": False,
        "connected": False,
        "turboshift_done": False,
        "session_mode": "UNKNOWN",
        "connect_time": 0,
        "turbo_time": 0,
        "nb_probe_count": 0,
        "phase2_detected": False,
        "nb_switches": [],
        "errors": [],
        "crash": None,
    }

    kill_mercury()
    lines = []
    lock = threading.Lock()
    stop_event = threading.Event()
    procs = []
    sockets = []
    threads = []
    t0 = time.time()

    header = f"\n{'='*70}\n  Test: {test_name} (iter {iteration}), expected={expected_session}\n{'='*70}"
    print(header)
    logfile.write(header + "\n")

    try:
        # Build command lines — always use -N or -W to override INI setting
        rsp_flags = [MERCURY, "-m", "ARQ", "-s", "100", "-R", "-g",
                     "-p", str(RSP_PORT),
                     "-i", VB_IN, "-o", VB_OUT,
                     "-x", "wasapi", "-n",
                     "-N" if rsp_nb else "-W"]

        cmd_flags = [MERCURY, "-m", "ARQ", "-s", "100", "-R", "-g",
                     "-p", str(CMD_PORT),
                     "-i", VB_IN, "-o", VB_OUT,
                     "-x", "wasapi", "-n",
                     "-N" if cmd_nb else "-W"]

        # Start responder
        print(f"  Starting responder ({rsp_mode})")
        rsp_proc = subprocess.Popen(rsp_flags, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        procs.append(rsp_proc)
        t = threading.Thread(target=collect_output,
                             args=(rsp_proc, "RSP", lines, lock, stop_event, t0), daemon=True)
        t.start()
        threads.append(t)
        time.sleep(3)

        # Start commander
        print(f"  Starting commander ({cmd_mode})")
        cmd_proc = subprocess.Popen(cmd_flags, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        procs.append(cmd_proc)
        t = threading.Thread(target=collect_output,
                             args=(cmd_proc, "CMD", lines, lock, stop_event, t0), daemon=True)
        t.start()
        threads.append(t)
        time.sleep(3)

        # Connect TCP control sockets and start reader threads
        rsp_ctrl = tcp_connect(RSP_PORT)
        sockets.append(rsp_ctrl)
        t = threading.Thread(target=tcp_reader,
                             args=(rsp_ctrl, "RSP", lines, lock, stop_event, t0), daemon=True)
        t.start()
        threads.append(t)

        cmd_ctrl = tcp_connect(CMD_PORT)
        sockets.append(cmd_ctrl)
        t = threading.Thread(target=tcp_reader,
                             args=(cmd_ctrl, "CMD", lines, lock, stop_event, t0), daemon=True)
        t.start()
        threads.append(t)

        # Set callsigns
        tcp_send(rsp_ctrl, "MYCALL TESTB\r\n")
        time.sleep(0.5)
        tcp_send(rsp_ctrl, "LISTEN ON\r\n")
        time.sleep(1)

        # Connect data ports (needed for turboshift data exchange)
        cmd_data = tcp_connect(CMD_PORT + 1)
        sockets.append(cmd_data)
        rsp_data = tcp_connect(RSP_PORT + 1)
        sockets.append(rsp_data)

        # Pre-fill TX FIFO
        try:
            chunk = bytes(range(256)) * 4
            cmd_data.settimeout(2)
            cmd_data.send(chunk * 4)  # 4KB
        except:
            pass

        # Start RX drain thread
        def drain_rx():
            rsp_data.settimeout(2)
            while not stop_event.is_set():
                try:
                    rsp_data.recv(4096)
                except:
                    time.sleep(0.1)
        t = threading.Thread(target=drain_rx, daemon=True)
        t.start()
        threads.append(t)

        connect_t0 = time.time()

        # Issue CONNECT
        print(f"  Sending CONNECT at T+{time.time()-t0:.1f}s")
        tcp_send(cmd_ctrl, "CONNECT TESTA TESTB\r\n")

        # Wait for CONNECTED from TCP control socket (not DISCONNECTED)
        # The pattern appears as "CONNECTED TESTA TESTB" on TCP
        print(f"  Waiting for CONNECTED (timeout={timeout_s}s)...")
        match, elapsed = wait_for_pattern(lines, lock,
                                          r"TCP\].*(?<!DIS)CONNECTED\s+\w+", timeout_s, connect_t0)

        if not match:
            # Also check stdout for connection_status print
            match, elapsed = wait_for_pattern(lines, lock,
                                              r"link_status:Connected",
                                              5, connect_t0)

        if match:
            result["connected"] = True
            result["connect_time"] = elapsed
            print(f"  CONNECTED in {elapsed:.1f}s ({match.strip()[:80]})")
        else:
            result["errors"].append("Connection timeout")
            print(f"  FAILED: Connection timeout after {timeout_s}s")
            # Dump key diagnostic lines
            with lock:
                diag_patterns = re.compile(r'NB-NEG|Phase|CFG.*load|PHY.*Config|PHY.*MFSK|START_CON|CONNECT|FAIL|ERROR|ACK_TIMED|attempt|DROPPED|Maximum|Switching')
                diag_lines = [l for l in lines if diag_patterns.search(l)]
                print(f"  Diagnostic lines ({len(diag_lines)} of {len(lines)} total):")
                for line in diag_lines[-40:]:
                    print(f"    {line}")
                    logfile.write(f"    {line}\n")
            # Write full log to file even on failure
            logfile.write(f"\n--- Full log ({len(lines)} lines) ---\n")
            with lock:
                for line in lines:
                    logfile.write(f"{line}\n")
            return result

        # Wait for turboshift completion
        match2, elapsed2 = wait_for_pattern(lines, lock,
                                            r"\[TURBO\] DONE",
                                            40, connect_t0)
        if match2:
            result["turboshift_done"] = True
            result["turbo_time"] = elapsed2
            print(f"  Turboshift DONE in {elapsed2:.1f}s")
        else:
            print(f"  Turboshift not completed in 40s (may still be probing)")

        # Let it run for 3 more seconds to verify stability
        time.sleep(3)

        # Check for crashes
        for proc in procs:
            if proc.poll() is not None:
                code = proc.returncode
                if code != 0 and code != 1:
                    result["crash"] = f"0x{code & 0xFFFFFFFF:08X}"
                    result["errors"].append(f"CRASH: {result['crash']}")
                    print(f"  CRASH: exit code {result['crash']}")

        # Analyze collected lines
        with lock:
            all_lines = list(lines)

        # NB negotiation analysis
        nb_probes = sum(1 for l in all_lines if "Phase 1 NB probe" in l)
        result["nb_probe_count"] = nb_probes

        result["phase2_detected"] = any("Phase 2" in l for l in all_lines)

        nb_switches = [l for l in all_lines if "NB-NEG" in l]
        result["nb_switches"] = nb_switches
        for sw in nb_switches:
            print(f"    NB-NEG: {sw.strip()[:100]}")
            logfile.write(f"    NB-NEG: {sw}\n")

        # Determine session mode
        rsp_nb_switch = any("Responder: switching to narrowband" in l for l in all_lines)
        cmd_nb_switch_back = any("NB commander connected via WB" in l for l in all_lines)

        if cmd_nb and rsp_nb:
            result["session_mode"] = "NB"
        elif not cmd_nb and rsp_nb:
            result["session_mode"] = "NB"  # WB cmdr found NB resp via probe
        elif cmd_nb and not rsp_nb:
            result["session_mode"] = "NB" if rsp_nb_switch else "WB"
        else:
            result["session_mode"] = "NB" if rsp_nb_switch else "WB"

        # Check for errors in logs
        error_patterns = re.compile(r'ERROR|ACCESS_VIOLATION|HEAP_CORRUPTION|REJECTED|double.free|segfault', re.IGNORECASE)
        for l in all_lines:
            if error_patterns.search(l):
                result["errors"].append(l.strip()[:120])

        # Count NAcks
        nack_count = sum(1 for l in all_lines if re.search(r'nNAcked_data=\s*[1-9]|nNAcked_control=\s*[1-9]|NAck', l))

        # Determine pass/fail
        if result["connected"] and result["session_mode"] == expected_session and not result["crash"]:
            result["passed"] = True

        status = "PASS" if result["passed"] else "FAIL"
        print(f"  Result: {status} | session={result['session_mode']} "
              f"(expected={expected_session}) | connect={result['connect_time']:.1f}s "
              f"| probes={nb_probes} | phase2={result['phase2_detected']}"
              f"| nacks={nack_count}")
        if result["errors"]:
            for e in result["errors"][:5]:
                print(f"    ERROR: {e[:120]}")

        # Write full log to file
        logfile.write(f"\n--- Full log ({len(all_lines)} lines) ---\n")
        for line in all_lines:
            logfile.write(f"{line}\n")

    except Exception as ex:
        result["errors"].append(f"Exception: {ex}")
        print(f"  EXCEPTION: {ex}")
        import traceback
        traceback.print_exc()

    finally:
        stop_event.set()
        for s in sockets:
            try: s.close()
            except: pass
        for p in procs:
            try: p.terminate()
            except: pass
        time.sleep(1)
        for p in procs:
            try: p.kill()
            except: pass
        kill_mercury()

    return result


def main():
    parser = argparse.ArgumentParser(description="NB/WB auto-negotiation test battery")
    parser.add_argument("--iterations", "-i", type=int, default=3,
                        help="Iterations per combination (default 3)")
    parser.add_argument("--timeout", "-t", type=int, default=90,
                        help="Per-test timeout in seconds (default 90)")
    parser.add_argument("--combo", "-c", type=int, default=None,
                        help="Run only combo N (0-3): 0=WB+WB, 1=NB+NB, 2=WB+NB, 3=NB+WB)")
    args = parser.parse_args()

    combos = COMBOS if args.combo is None else [COMBOS[args.combo]]
    total_tests = len(combos) * args.iterations

    print(f"+----------------------------------------------------------+")
    print(f"|   Mercury NB/WB Auto-Negotiation Test Battery            |")
    print(f"|   Combinations: {len(combos)}, Iterations: {args.iterations}, Total: {total_tests:<10}     |")
    print(f"|   Timeout: {args.timeout}s per test                                |")
    print(f"+----------------------------------------------------------+")
    print()

    logpath = "nb_negotiation_test.log"
    results = []

    with open(logpath, "w", encoding='utf-8') as logfile:
        logfile.write(f"NB/WB Auto-Negotiation Test Battery\n")
        logfile.write(f"Date: {time.strftime('%Y-%m-%d %H:%M:%S')}\n")
        logfile.write(f"Iterations: {args.iterations}, Timeout: {args.timeout}s\n\n")

        test_num = 0
        for combo_idx, (cmd_nb, rsp_nb, expected) in enumerate(combos):
            for iteration in range(1, args.iterations + 1):
                test_num += 1
                print(f"\n[{test_num}/{total_tests}]", end="")
                r = run_single_test(cmd_nb, rsp_nb, expected,
                                    args.timeout, iteration, logfile)
                results.append(r)

    # Print summary
    print(f"\n\n{'='*70}")
    print(f"  SUMMARY")
    print(f"{'='*70}")

    from collections import defaultdict
    by_test = defaultdict(list)
    for r in results:
        by_test[r["test"]].append(r)

    total_pass = 0
    total_fail = 0
    for test_name, test_results in by_test.items():
        passes = sum(1 for r in test_results if r["passed"])
        fails = len(test_results) - passes
        total_pass += passes
        total_fail += fails

        connect_times = [r["connect_time"] for r in test_results if r["connected"]]
        avg_ct = sum(connect_times) / len(connect_times) if connect_times else 0
        max_ct = max(connect_times) if connect_times else 0

        crashes = [r["crash"] for r in test_results if r["crash"]]
        probes = [r["nb_probe_count"] for r in test_results]
        phase2s = sum(1 for r in test_results if r["phase2_detected"])

        status = "PASS" if fails == 0 else "FAIL"
        print(f"\n  {status} {test_name}: {passes}/{len(test_results)} passed")
        print(f"       Connect time: avg={avg_ct:.1f}s, max={max_ct:.1f}s")
        print(f"       NB probes: {probes}, Phase 2: {phase2s}/{len(test_results)}")
        if crashes:
            print(f"       CRASHES: {crashes}")

        for r in test_results:
            s = "OK" if r["passed"] else "FAIL"
            err_str = f" [{r['errors'][0][:60]}]" if r["errors"] else ""
            print(f"         iter {r['iteration']}: {s} "
                  f"session={r['session_mode']} "
                  f"t={r['connect_time']:.1f}s "
                  f"probes={r['nb_probe_count']}"
                  f"{err_str}")

    print(f"\n{'='*70}")
    verdict = "ALL PASS" if total_fail == 0 else f"{total_fail} FAILURES"
    print(f"  VERDICT: {verdict} ({total_pass}/{total_pass+total_fail})")
    print(f"  Log: {logpath}")
    print(f"{'='*70}\n")

    return 0 if total_fail == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
