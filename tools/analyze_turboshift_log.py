#!/usr/bin/env python3
"""
Mercury Turboshift Log Analyzer
================================
Parses timestamped logs from robust_loopback_test.py and extracts timing chains
for each SET_CONFIG transition during turboshift probing.

Usage:
    python analyze_turboshift_log.py <logfile> [options]

Options:
    --nacks      Only show transitions that NAcked
    --summary    Only show per-transition summary table
    --transition N  Show detailed timeline for transition N (0-indexed)
    --raw        Show all parsed events (verbose)
    --search-window  Analyze coarse search window coverage for each decode attempt

Log format expected (from robust_loopback_test.py with timestamps):
    [T+000082.300] [CMD] [TX-END] frames_to_read=18 (ctrl=0)
    [T+000082.900] [RSP] [BUF-ENERGY] nUnder=37 | 0.000 0.000 0.497 ...

Also supports legacy format (without timestamps) — uses line numbers as proxy.

Key timing events tracked:
    CMD: TURBO UP, TX send_batch, PTT ON/OFF, TX-END, ACK-RX polls, ACK-DET,
         GEARSHIFT SET_CONFIG, CMD-RX entering receive, TIMEOUT/NAck
    RSP: BUF-ENERGY, OFDM-SYNC coarse/bounds-skip/silence-skip/fine-energy-fix,
         CHAN-EST, RX-TIMING OK/FAIL, TX-ACK-PAT start/done, PHY-REINIT,
         SKIP-H, trial results

Output: per-transition timeline with millisecond deltas, highlighting failures.
"""

import re
import sys
import argparse
from dataclasses import dataclass, field
from typing import Optional


# -- Regex patterns for log parsing ------------------------------------------

# Timestamp: [T+000082.300] or legacy [CMD-TCP @ 82.3s]
RE_TIMESTAMP = re.compile(r'^\[T\+(\d+\.\d+)\]\s+')
RE_LEGACY_TCP = re.compile(r'\[(?:CMD|RSP)-TCP\s+@\s+([\d.]+)s\]')

# Role prefix: [CMD] or [RSP]
RE_ROLE = re.compile(r'\[(CMD|RSP)\]')

# -- CMD events --
RE_TURBO_UP = re.compile(r'\[TURBO\] UP: config (\d+) -> (\d+)')
RE_TURBO_CEILING = re.compile(r'\[TURBO\].*CEILING.*config (\d+).*settling.*?(\d+)')
RE_TURBO_FORWARD = re.compile(r'\[TURBO\].*FORWARD')
RE_TURBO_REVERSE = re.compile(r'\[TURBO\].*REVERSE')
RE_TURBO_DONE = re.compile(r'\[TURBO\].*DONE')
RE_SET_CONFIG = re.compile(r'\[GEARSHIFT\] SET_CONFIG: forward=(\d+) reverse=(\d+)')
RE_SET_CONFIG_ACKED = re.compile(r'\[GEARSHIFT\] SET_CONFIG ACKed, loaded config (\d+)')
RE_TX_BATCH = re.compile(r'\[TX\] send_batch\(\) on (CONFIG_\d+|CONFIG_10[012]), (\d+) messages')
RE_PTT_ON = re.compile(r'\[(?:CMD|RSP)-TCP\].*PTT ON|PTT ON')
RE_PTT_OFF = re.compile(r'\[(?:CMD|RSP)-TCP\].*PTT OFF|PTT OFF')
RE_TX_END = re.compile(r'\[TX-END\] frames_to_read=(\d+)')
RE_CMD_RX = re.compile(r'\[CMD-RX\].*recv_timeout=(\d+).*msg_tx_time=(\d+).*ftr=(\d+)')
RE_ACK_RX = re.compile(r'\[ACK-RX\].*matched=(\d+)/16')
RE_ACK_DET = re.compile(r'\[ACK-DET\].*matched=(\d+)/16')
RE_CMD_ACK_PAT = re.compile(r'\[CMD-ACK-PAT\]')
RE_NACKED = re.compile(r'nNAcked_control=\s*(\d+)')
RE_SWITCH_ROLE = re.compile(r'\[TURBO\].*SWITCH_ROLE|SWITCH_ROLE')

# -- RSP events --
RE_BUF_ENERGY = re.compile(r'\[BUF-ENERGY\] nUnder=(\d+) \| ([\d. ]+)')
RE_COARSE = re.compile(r'\[OFDM-SYNC\] coarse: pream_symb=(\d+) delay=(\d+) bounds=\[(\d+),(\d+)\] metric=([\d.]+) (PASS|SKIP)')
RE_BOUNDS_SKIP = re.compile(r'\[OFDM-SYNC\] bounds-skip: signal=(\d+) retry=(\d+) metric=([\d.]+) energy=([\d.e+-]+)')
RE_BOUNDS_FAILED = re.compile(r'\[OFDM-SYNC\] bounds-failed')
RE_SILENCE_SKIP = re.compile(r'\[OFDM-SYNC\] silence-skip: orig=(\d+) signal=(\d+) retry=(\d+) metric=([\d.]+) energy=([\d.e+-]+)')
RE_ENERGY_FIX = re.compile(r'\[OFDM-SYNC\] fine-energy-fix: delay (\d+)->(\d+)')
RE_METRIC_WEAK = re.compile(r'\[OFDM-SYNC\] metric=([\d.]+).*weak peak')
RE_CHAN_EST = re.compile(r'\[CHAN-EST\].*mean_H=([\d.]+)')
RE_SKIP_H = re.compile(r'SKIP-H.*mean_H=([\d.]+)')
RE_RX_TIMING_OK = re.compile(r'\[RX-TIMING\] OK:.*delay_symb=(\d+).*nUnder=(\d+).*ftr=(-?\d+).*proc=(\d+)ms')
RE_RX_TIMING_FAIL = re.compile(r'\[RX-TIMING\] FAIL:.*nUnder=(\d+).*proc=(\d+)ms')
RE_TX_ACK_START = re.compile(r'\[TX-ACK-PAT\] Sending ACK pattern on (CONFIG_\d+|CONFIG_10[012])')
RE_TX_ACK_DONE = re.compile(r'\[TX-ACK-PAT\] Done, flushed capture buffer')
RE_PHY_REINIT = re.compile(r'\[PHY-REINIT\] About to deinit.*config (\d+) -> (\d+)')
RE_PHY_ACTIVE = re.compile(r'\[PHY\] Config (\d+) active: M=(\d+).*Nsymb=(\d+)')
RE_TRIAL_RESULT = re.compile(r'\[OFDM-SYNC\] trial (\d+) (FAIL|OK):.*delay=(\d+).*iter=(\d+)')
RE_GEARSHIFT_NACK = re.compile(r'\[GEARSHIFT\].*NAck|nack_timeout|gear_shift_timer.*expired')

# -- PHY-REINIT lifecycle events --
RE_DEINIT_START = re.compile(r'\[PHY-REINIT\] Mutex zeroed, calling deinit')
RE_DEINIT_COMPLETE = re.compile(r'\[PHY-REINIT\] deinit complete')
RE_INIT_START = re.compile(r'\[PHY-REINIT\] Calling init')
RE_INIT_COMPLETE = re.compile(r'\[PHY-REINIT\] init complete')

# -- Process exit / crash --
RE_PROCESS_EXIT = re.compile(r'exited with code (\d+)')
RE_ACK_CTRL_LOAD = re.compile(r'\[ACK-CTRL\] Loading new data config (\d+) \(was (\d+)\)')

# Config properties (from ACK_TIMING_ANALYSIS.md Section 5)
CONFIG_PROPS = {
    100: {"name": "ROBUST_0", "mod": "32-MFSK", "nsymb": None, "preamble": 4, "tx_ms": None},
    101: {"name": "ROBUST_1", "mod": "16-MFSK", "nsymb": None, "preamble": 4, "tx_ms": None},
    102: {"name": "ROBUST_2", "mod": "16-MFSK", "nsymb": None, "preamble": 4, "tx_ms": None},
    0:  {"name": "CONFIG_0",  "mod": "BPSK",  "nsymb": 48, "preamble": 4, "tx_ms": 1179},
    1:  {"name": "CONFIG_1",  "mod": "BPSK",  "nsymb": 48, "preamble": 4, "tx_ms": 1179},
    2:  {"name": "CONFIG_2",  "mod": "BPSK",  "nsymb": 48, "preamble": 4, "tx_ms": 1179},
    3:  {"name": "CONFIG_3",  "mod": "BPSK",  "nsymb": 48, "preamble": 4, "tx_ms": 1179},
    4:  {"name": "CONFIG_4",  "mod": "BPSK",  "nsymb": 48, "preamble": 4, "tx_ms": 1179},
    5:  {"name": "CONFIG_5",  "mod": "BPSK",  "nsymb": 48, "preamble": 4, "tx_ms": 1179},
    6:  {"name": "CONFIG_6",  "mod": "BPSK",  "nsymb": 48, "preamble": 4, "tx_ms": 1179},
    7:  {"name": "CONFIG_7",  "mod": "QPSK",  "nsymb": 24, "preamble": 4, "tx_ms": 635},
    8:  {"name": "CONFIG_8",  "mod": "QPSK",  "nsymb": 24, "preamble": 4, "tx_ms": 635},
    9:  {"name": "CONFIG_9",  "mod": "QPSK",  "nsymb": 24, "preamble": 4, "tx_ms": 635},
    10: {"name": "CONFIG_10", "mod": "8PSK",  "nsymb": 16, "preamble": 3, "tx_ms": 431},
    11: {"name": "CONFIG_11", "mod": "8PSK",  "nsymb": 16, "preamble": 3, "tx_ms": 431},
    12: {"name": "CONFIG_12", "mod": "QPSK",  "nsymb": 24, "preamble": 3, "tx_ms": 612},
    13: {"name": "CONFIG_13", "mod": "16QAM", "nsymb": 12, "preamble": 2, "tx_ms": 317},
    14: {"name": "CONFIG_14", "mod": "8PSK",  "nsymb": 16, "preamble": 2, "tx_ms": 408},
    15: {"name": "CONFIG_15", "mod": "16QAM", "nsymb": 12, "preamble": 2, "tx_ms": 317},
    16: {"name": "CONFIG_16", "mod": "32QAM", "nsymb": 9,  "preamble": 1, "tx_ms": 227},
}


def config_num(s):
    """Parse 'CONFIG_14' or '14' or '100' to int."""
    if s.startswith("CONFIG_"):
        return int(s[7:])
    return int(s)


def config_name(n):
    """Get human-readable name for config number."""
    if n in CONFIG_PROPS:
        return CONFIG_PROPS[n]["name"]
    return f"CONFIG_{n}"


@dataclass
class Event:
    """A single parsed log event."""
    time: float          # seconds from T=0 (or line number if no timestamp)
    line_num: int        # original line number in log
    role: str            # "CMD" or "RSP"
    event_type: str      # e.g., "TURBO_UP", "BUF_ENERGY", "COARSE", etc.
    data: dict = field(default_factory=dict)
    raw: str = ""


@dataclass
class CrashInfo:
    """Detected process crash (silent death during deinit)."""
    role: str = ""                 # "CMD" or "RSP"
    crash_time: float = -1         # time of last line from crashed process
    crash_line: int = 0            # line number of last line
    crash_raw: str = ""            # last line text
    deinit_config_from: int = -1   # config being deinited
    deinit_config_to: int = -1     # config being loaded
    other_role_last_time: float = -1  # last timestamp from surviving process
    other_role_last_line: int = 0
    silence_duration: float = 0    # how long crashed process was silent


@dataclass
class DecodeAttempt:
    """One RSP buffer fill + decode attempt."""
    time: float
    nUnder: int = 0
    energy_chunks: str = ""
    coarse_pream: int = -1
    coarse_delay: int = -1
    coarse_metric: float = 0.0
    coarse_result: str = ""  # PASS/SKIP
    bounds_lo: int = 0
    bounds_hi: int = 0
    recovery_type: str = ""   # bounds-skip, silence-skip, fine-energy-fix
    recovery_metric: float = 0.0
    recovery_energy: float = 0.0
    recovery_symb: int = -1
    mean_H: float = -1.0
    result: str = ""     # OK, FAIL, SKIP-H
    delay_symb: int = -1
    proc_ms: int = 0
    ftr: int = 0
    trials: list = field(default_factory=list)


@dataclass
class Transition:
    """One SET_CONFIG transition (CMD TX -> RSP decode -> ACK -> CMD receives ACK)."""
    index: int = 0
    config_from: int = -1
    config_to: int = -1
    direction: str = ""     # FORWARD or REVERSE

    # CMD TX
    cmd_turbo_up_time: float = -1
    cmd_tx_start: float = -1     # PTT ON
    cmd_tx_end: float = -1       # PTT OFF / TX-END
    cmd_rx_enter: float = -1     # CMD-RX entering receive
    cmd_recv_timeout: int = 0
    cmd_msg_tx_time: int = 0

    # RSP decode
    rsp_attempts: list = field(default_factory=list)
    rsp_decode_ok: bool = False
    rsp_decode_time: float = -1

    # RSP ACK
    rsp_ack_start: float = -1
    rsp_ack_done: float = -1
    rsp_ack_config: str = ""

    # RSP config load
    rsp_reinit_time: float = -1
    rsp_reinit_done: float = -1

    # CMD ACK detection
    cmd_ack_polls: list = field(default_factory=list)  # (time, matched_count)
    cmd_ack_detected: float = -1

    # Result
    success: bool = False
    nack: bool = False
    retry: bool = False
    cmd_timeout_time: float = -1

    # Coarse search window analysis
    search_window_symb: int = 0   # how many symbols the coarse search covers
    buffer_nsymb: int = 0


def parse_log(filepath):
    """Parse log file into list of Events."""
    events = []
    has_timestamps = False

    with open(filepath, 'r', encoding='utf-8', errors='replace') as f:
        for line_num, raw_line in enumerate(f, 1):
            line = raw_line.rstrip()
            if not line:
                continue

            # Extract timestamp
            ts = None
            m = RE_TIMESTAMP.match(line)
            if m:
                ts = float(m.group(1))
                has_timestamps = True
                line_content = line[m.end():]
            else:
                # Legacy: use line number as proxy
                ts = line_num * 0.001  # ~1ms per line (rough proxy)
                line_content = line

            # Check for legacy TCP timestamp
            m_tcp = RE_LEGACY_TCP.search(line)
            if m_tcp and not has_timestamps:
                ts = float(m_tcp.group(1))

            # Extract role
            m_role = RE_ROLE.search(line_content)
            role = m_role.group(1) if m_role else "???"

            # Match against all patterns
            evt = None

            # CMD events
            m = RE_TURBO_UP.search(line_content)
            if m:
                evt = Event(ts, line_num, role, "TURBO_UP",
                           {"from": int(m.group(1)), "to": int(m.group(2))}, line)

            if not evt:
                m = RE_TURBO_CEILING.search(line_content)
                if m:
                    evt = Event(ts, line_num, role, "TURBO_CEILING",
                               {"at": int(m.group(1)), "settled": int(m.group(2))}, line)

            if not evt:
                m = RE_SET_CONFIG.search(line_content)
                if m:
                    evt = Event(ts, line_num, role, "SET_CONFIG_SEND",
                               {"forward": int(m.group(1)), "reverse": int(m.group(2))}, line)

            if not evt:
                m = RE_SET_CONFIG_ACKED.search(line_content)
                if m:
                    evt = Event(ts, line_num, role, "SET_CONFIG_ACKED",
                               {"config": int(m.group(1))}, line)

            if not evt:
                m = RE_CMD_RX.search(line_content)
                if m:
                    evt = Event(ts, line_num, role, "CMD_RX_ENTER",
                               {"recv_timeout": int(m.group(1)),
                                "msg_tx_time": int(m.group(2)),
                                "ftr": int(m.group(3))}, line)

            if not evt:
                m = RE_ACK_DET.search(line_content)
                if m:
                    evt = Event(ts, line_num, role, "ACK_DET",
                               {"matched": int(m.group(1))}, line)

            if not evt:
                m = RE_ACK_RX.search(line_content)
                if m:
                    evt = Event(ts, line_num, role, "ACK_RX",
                               {"matched": int(m.group(1))}, line)

            if not evt and RE_CMD_ACK_PAT.search(line_content):
                evt = Event(ts, line_num, role, "CMD_ACK_DETECTED", {}, line)

            if not evt:
                m = RE_TX_END.search(line_content)
                if m:
                    evt = Event(ts, line_num, role, "TX_END",
                               {"ftr": int(m.group(1))}, line)

            # RSP events
            if not evt:
                m = RE_BUF_ENERGY.search(line_content)
                if m:
                    evt = Event(ts, line_num, role, "BUF_ENERGY",
                               {"nUnder": int(m.group(1)),
                                "chunks": m.group(2).strip()}, line)

            if not evt:
                m = RE_COARSE.search(line_content)
                if m:
                    evt = Event(ts, line_num, role, "COARSE",
                               {"pream_symb": int(m.group(1)),
                                "delay": int(m.group(2)),
                                "lo": int(m.group(3)),
                                "hi": int(m.group(4)),
                                "metric": float(m.group(5)),
                                "result": m.group(6)}, line)

            if not evt:
                m = RE_BOUNDS_SKIP.search(line_content)
                if m:
                    evt = Event(ts, line_num, role, "BOUNDS_SKIP",
                               {"signal": int(m.group(1)),
                                "retry": int(m.group(2)),
                                "metric": float(m.group(3)),
                                "energy": float(m.group(4))}, line)

            if not evt and RE_BOUNDS_FAILED.search(line_content):
                evt = Event(ts, line_num, role, "BOUNDS_FAILED", {}, line)

            if not evt:
                m = RE_SILENCE_SKIP.search(line_content)
                if m:
                    evt = Event(ts, line_num, role, "SILENCE_SKIP",
                               {"orig": int(m.group(1)),
                                "signal": int(m.group(2)),
                                "retry": int(m.group(3)),
                                "metric": float(m.group(4)),
                                "energy": float(m.group(5))}, line)

            if not evt:
                m = RE_ENERGY_FIX.search(line_content)
                if m:
                    evt = Event(ts, line_num, role, "ENERGY_FIX",
                               {"from": int(m.group(1)), "to": int(m.group(2))}, line)

            if not evt:
                m = RE_METRIC_WEAK.search(line_content)
                if m:
                    evt = Event(ts, line_num, role, "METRIC_WEAK",
                               {"metric": float(m.group(1))}, line)

            if not evt:
                m = RE_RX_TIMING_OK.search(line_content)
                if m:
                    evt = Event(ts, line_num, role, "RX_TIMING_OK",
                               {"delay_symb": int(m.group(1)),
                                "nUnder": int(m.group(2)),
                                "ftr": int(m.group(3)),
                                "proc_ms": int(m.group(4))}, line)

            if not evt:
                m = RE_RX_TIMING_FAIL.search(line_content)
                if m:
                    evt = Event(ts, line_num, role, "RX_TIMING_FAIL",
                               {"nUnder": int(m.group(1)),
                                "proc_ms": int(m.group(2))}, line)

            if not evt:
                m = RE_TX_ACK_START.search(line_content)
                if m:
                    evt = Event(ts, line_num, role, "TX_ACK_START",
                               {"config": m.group(1)}, line)

            if not evt:
                m = RE_TX_ACK_DONE.search(line_content)
                if m:
                    evt = Event(ts, line_num, role, "TX_ACK_DONE", {}, line)

            if not evt:
                m = RE_PHY_REINIT.search(line_content)
                if m:
                    evt = Event(ts, line_num, role, "PHY_REINIT",
                               {"from": int(m.group(1)), "to": int(m.group(2))}, line)

            if not evt:
                m = RE_PHY_ACTIVE.search(line_content)
                if m:
                    evt = Event(ts, line_num, role, "PHY_ACTIVE",
                               {"config": int(m.group(1)),
                                "M": int(m.group(2)),
                                "Nsymb": int(m.group(3))}, line)

            if not evt:
                m = RE_TRIAL_RESULT.search(line_content)
                if m:
                    evt = Event(ts, line_num, role, "TRIAL_RESULT",
                               {"trial": int(m.group(1)),
                                "result": m.group(2),
                                "delay": int(m.group(3)),
                                "iter": int(m.group(4))}, line)

            if not evt:
                m = RE_CHAN_EST.search(line_content)
                if m:
                    evt = Event(ts, line_num, role, "CHAN_EST",
                               {"mean_H": float(m.group(1))}, line)

            # PHY-REINIT lifecycle events
            if not evt and RE_DEINIT_START.search(line_content):
                evt = Event(ts, line_num, role, "DEINIT_START", {}, line)

            if not evt and RE_DEINIT_COMPLETE.search(line_content):
                evt = Event(ts, line_num, role, "DEINIT_COMPLETE", {}, line)

            if not evt and RE_INIT_START.search(line_content):
                evt = Event(ts, line_num, role, "INIT_START", {}, line)

            if not evt and RE_INIT_COMPLETE.search(line_content):
                evt = Event(ts, line_num, role, "INIT_COMPLETE", {}, line)

            if not evt:
                m = RE_PROCESS_EXIT.search(line_content)
                if m:
                    evt = Event(ts, line_num, role, "PROCESS_EXIT",
                               {"code": int(m.group(1))}, line)

            if not evt:
                m = RE_ACK_CTRL_LOAD.search(line_content)
                if m:
                    evt = Event(ts, line_num, role, "ACK_CTRL_LOAD",
                               {"to": int(m.group(1)), "from": int(m.group(2))}, line)

            # PTT events (from TCP)
            if not evt and 'PTT ON' in line_content:
                evt = Event(ts, line_num, role if role != "???" else
                           ("CMD" if "CMD" in line_content else "RSP"),
                           "PTT_ON", {}, line)

            if not evt and 'PTT OFF' in line_content:
                evt = Event(ts, line_num, role if role != "???" else
                           ("CMD" if "CMD" in line_content else "RSP"),
                           "PTT_OFF", {}, line)

            if evt:
                events.append(evt)

    return events, has_timestamps


def build_transitions(events):
    """Group events into Transition objects following the turboshift flow.

    Handles SWITCH_ROLE: after a role swap, the process labeled [RSP] becomes
    the ARQ commander (sends TURBO_UP, TX, ACK polling) and [CMD] becomes the
    responder (BUF_ENERGY, OFDM-SYNC, TX_ACK). We track this by following which
    process label is currently the "commander" vs "responder".
    """
    transitions = []
    current = None
    current_attempt = None
    direction = "FORWARD"
    trans_idx = 0
    # Track which process label is currently commander/responder
    # Initially: CMD process = commander, RSP process = responder
    commander_label = "CMD"
    responder_label = "RSP"

    for evt in events:
        # Track direction changes
        if evt.event_type == "TURBO_UP" and RE_TURBO_FORWARD.search(evt.raw):
            direction = "FORWARD"
        elif RE_TURBO_REVERSE.search(evt.raw):
            direction = "REVERSE"

        # TURBO_UP tells us which process is currently the commander
        if evt.event_type == "TURBO_UP":
            commander_label = evt.role
            responder_label = "RSP" if evt.role == "CMD" else "CMD"

        # Start of a new transition: TURBO_UP
        if evt.event_type == "TURBO_UP":
            if current and not current.success and not current.nack:
                # Previous transition wasn't resolved — mark as nack
                current.nack = True
            current = Transition(
                index=trans_idx,
                config_from=evt.data["from"],
                config_to=evt.data["to"],
                direction=direction,
                cmd_turbo_up_time=evt.time,
            )
            current_attempt = None
            trans_idx += 1
            transitions.append(current)
            continue

        if current is None:
            continue

        # Determine if this event is from the current commander or responder
        is_commander = (evt.role == commander_label)
        is_responder = (evt.role == responder_label)

        # Commander events (whichever process is currently commander)
        if is_commander:
            if evt.event_type == "PTT_ON" and current.cmd_tx_start < 0:
                current.cmd_tx_start = evt.time

            elif evt.event_type == "PTT_OFF" and current.cmd_tx_end < 0:
                current.cmd_tx_end = evt.time

            elif evt.event_type == "TX_END" and current.cmd_tx_end < 0:
                current.cmd_tx_end = evt.time

            elif evt.event_type == "CMD_RX_ENTER":
                current.cmd_rx_enter = evt.time
                current.cmd_recv_timeout = evt.data["recv_timeout"]
                current.cmd_msg_tx_time = evt.data["msg_tx_time"]

            elif evt.event_type in ("ACK_RX", "ACK_DET"):
                current.cmd_ack_polls.append((evt.time, evt.data["matched"]))

            elif evt.event_type == "CMD_ACK_DETECTED":
                current.cmd_ack_detected = evt.time

            elif evt.event_type == "SET_CONFIG_ACKED":
                current.success = True
                current.rsp_decode_ok = True

            elif evt.event_type == "TURBO_CEILING":
                current.nack = True

        # Responder events (whichever process is currently responder)
        if is_responder:
            if evt.event_type == "BUF_ENERGY":
                # Start a new decode attempt
                current_attempt = DecodeAttempt(
                    time=evt.time,
                    nUnder=evt.data["nUnder"],
                    energy_chunks=evt.data["chunks"],
                )
                current.rsp_attempts.append(current_attempt)

            elif evt.event_type == "COARSE" and current_attempt:
                current_attempt.coarse_pream = evt.data["pream_symb"]
                current_attempt.coarse_delay = evt.data["delay"]
                current_attempt.coarse_metric = evt.data["metric"]
                current_attempt.coarse_result = evt.data["result"]
                current_attempt.bounds_lo = evt.data["lo"]
                current_attempt.bounds_hi = evt.data["hi"]
                # Calculate coarse search window coverage
                props = CONFIG_PROPS.get(current.config_from, {})
                nsymb = props.get("nsymb", 0)
                preamble = props.get("preamble", 0)
                if nsymb and preamble:
                    current.search_window_symb = 2 * preamble + nsymb
                    current.buffer_nsymb = evt.data["hi"] + nsymb + preamble

            elif evt.event_type == "BOUNDS_SKIP" and current_attempt:
                current_attempt.recovery_type = "bounds-skip"
                current_attempt.recovery_metric = evt.data["metric"]
                current_attempt.recovery_energy = evt.data["energy"]
                current_attempt.recovery_symb = evt.data["retry"]

            elif evt.event_type == "SILENCE_SKIP" and current_attempt:
                current_attempt.recovery_type = "silence-skip"
                current_attempt.recovery_metric = evt.data["metric"]
                current_attempt.recovery_energy = evt.data["energy"]
                current_attempt.recovery_symb = evt.data["retry"]

            elif evt.event_type == "ENERGY_FIX" and current_attempt:
                current_attempt.recovery_type = "fine-energy-fix"

            elif evt.event_type == "METRIC_WEAK" and current_attempt:
                if not current_attempt.recovery_type:
                    current_attempt.recovery_type = "metric-weak"
                current_attempt.recovery_metric = evt.data["metric"]

            elif evt.event_type == "CHAN_EST" and current_attempt:
                current_attempt.mean_H = evt.data["mean_H"]

            elif evt.event_type == "TRIAL_RESULT" and current_attempt:
                current_attempt.trials.append(evt.data)

            elif evt.event_type == "RX_TIMING_OK" and current_attempt:
                current_attempt.result = "OK"
                current_attempt.delay_symb = evt.data["delay_symb"]
                current_attempt.proc_ms = evt.data["proc_ms"]
                current_attempt.ftr = evt.data["ftr"]
                current.rsp_decode_time = evt.time

            elif evt.event_type == "RX_TIMING_FAIL" and current_attempt:
                current_attempt.result = "FAIL"
                current_attempt.proc_ms = evt.data["proc_ms"]

            elif evt.event_type == "TX_ACK_START":
                current.rsp_ack_start = evt.time
                current.rsp_ack_config = evt.data["config"]

            elif evt.event_type == "TX_ACK_DONE":
                current.rsp_ack_done = evt.time

            elif evt.event_type == "PHY_REINIT":
                current.rsp_reinit_time = evt.time

    return transitions


def detect_crashes(events):
    """Detect silent process crashes by analyzing event patterns.

    Crash signature: process prints 'Mutex zeroed, calling deinit...' but never
    'deinit complete'. The process goes silent while the other continues.
    """
    crashes = []

    # Track deinit lifecycle per role
    # Key: role, Value: dict with pending deinit info
    pending_deinit = {}  # role -> {time, line, config_from, config_to}

    # Track last event time per role
    last_event = {}  # role -> (time, line_num, raw)

    # Track PHY_REINIT to get config_from/config_to for pending deinits
    last_reinit = {}  # role -> {from, to}

    for evt in events:
        if evt.role in ("CMD", "RSP"):
            last_event[evt.role] = (evt.time, evt.line_num, evt.raw)

        # Track PHY_REINIT (has config from/to)
        if evt.event_type == "PHY_REINIT":
            last_reinit[evt.role] = {"from": evt.data["from"], "to": evt.data["to"]}

        # Deinit started
        if evt.event_type == "DEINIT_START":
            reinit = last_reinit.get(evt.role, {"from": -1, "to": -1})
            pending_deinit[evt.role] = {
                "time": evt.time,
                "line": evt.line_num,
                "raw": evt.raw,
                "config_from": reinit["from"],
                "config_to": reinit["to"],
            }

        # Deinit completed — clear the pending
        if evt.event_type == "DEINIT_COMPLETE" and evt.role in pending_deinit:
            del pending_deinit[evt.role]

    # Check for any unresolved deinits (process died during deinit)
    for role, info in pending_deinit.items():
        other_role = "CMD" if role == "RSP" else "RSP"
        other_last = last_event.get(other_role, (-1, 0, ""))
        my_last = last_event.get(role, (-1, 0, ""))

        crash = CrashInfo(
            role=role,
            crash_time=info["time"],
            crash_line=info["line"],
            crash_raw=info["raw"],
            deinit_config_from=info["config_from"],
            deinit_config_to=info["config_to"],
            other_role_last_time=other_last[0],
            other_role_last_line=other_last[1],
            silence_duration=other_last[0] - info["time"] if other_last[0] > 0 else 0,
        )
        crashes.append(crash)

    # Also detect "gone silent" without deinit: process stops emitting events
    # while the other continues for >10s
    for role in ("CMD", "RSP"):
        if role in pending_deinit:
            continue  # already caught above
        other_role = "CMD" if role == "RSP" else "RSP"
        my_last = last_event.get(role, (-1, 0, ""))
        other_last = last_event.get(other_role, (-1, 0, ""))
        if my_last[0] > 0 and other_last[0] > 0:
            gap = other_last[0] - my_last[0]
            if gap > 10.0:  # >10s silence = likely crash
                crash = CrashInfo(
                    role=role,
                    crash_time=my_last[0],
                    crash_line=my_last[1],
                    crash_raw=my_last[2],
                    other_role_last_time=other_last[0],
                    other_role_last_line=other_last[1],
                    silence_duration=gap,
                )
                crashes.append(crash)

    return crashes


def print_crash_report(crashes, events):
    """Print crash detection report."""
    if not crashes:
        print(f"\n  {'='*70}")
        print(f"  CRASH DETECTION: No crashes detected")
        print(f"  {'='*70}")
        return

    for crash in crashes:
        exit_code_str = ""
        # Look for process exit events
        for evt in events:
            if evt.event_type == "PROCESS_EXIT":
                exit_code_str = f"  Exit code: {evt.data['code']} (0x{evt.data['code']:08X})"

        print(f"\n  {'='*70}")
        print(f"  CRASH DETECTED: {crash.role} process died!")
        print(f"  {'='*70}")
        if crash.deinit_config_from >= 0:
            print(f"  During: deinit() in config transition {config_name(crash.deinit_config_from)} -> {config_name(crash.deinit_config_to)}")
        print(f"  Last {crash.role} line: L{crash.crash_line} at T+{crash.crash_time:.3f}s")
        print(f"    {crash.crash_raw.strip()}")
        other = "CMD" if crash.role == "RSP" else "RSP"
        print(f"  {other} continued until: L{crash.other_role_last_line} at T+{crash.other_role_last_time:.3f}s")
        print(f"  {crash.role} silent for: {crash.silence_duration:.1f}s (while {other} continued)")
        if exit_code_str:
            print(exit_code_str)

        # Show context: last 5 events from crashed process before the crash
        role_events = [e for e in events if e.role == crash.role and e.time <= crash.crash_time + 0.001]
        tail = role_events[-8:] if len(role_events) > 8 else role_events
        print(f"\n  Last {len(tail)} {crash.role} events before crash:")
        for evt in tail:
            print(f"    L{evt.line_num:>6} T+{evt.time:.3f} {evt.event_type:20s} {evt.data if evt.data else ''}")

        print(f"\n  Root cause: Heap corruption in deinit() -> delete[] on corrupted pointer")
        print(f"  Signature: 'Mutex zeroed, calling deinit...' with no 'deinit complete'")
        print(f"  Windows: exit code 0xC0000005 = STATUS_ACCESS_VIOLATION")
        print(f"  {'='*70}")


def fmt_delta(t_from, t_to):
    """Format a time delta as +XXXms."""
    if t_from < 0 or t_to < 0:
        return "?ms"
    delta_ms = (t_to - t_from) * 1000
    return f"+{delta_ms:.0f}ms"


def fmt_time(t):
    """Format a timestamp as T+XXX.XXXs."""
    if t < 0:
        return "T+????"
    return f"T+{t:.3f}"


def print_transition_detail(tr, has_timestamps):
    """Print detailed timeline for one transition."""
    ts_note = "" if has_timestamps else " (timestamps approximate — line-number proxy)"
    from_name = config_name(tr.config_from)
    to_name = config_name(tr.config_to)
    props = CONFIG_PROPS.get(tr.config_from, {})
    mod = props.get("mod", "?")
    nsymb = props.get("nsymb", "?")
    preamble = props.get("preamble", "?")
    tx_ms = props.get("tx_ms", "?")

    result_str = "OK" if tr.success else ("NAck" if tr.nack else "PENDING")
    result_color = "" if tr.success else " <<<" if tr.nack else ""

    print(f"\n{'='*80}")
    print(f"  Transition #{tr.index}: {from_name} -> {to_name}  [{tr.direction}]  {result_str}{result_color}")
    print(f"  TX config: {from_name} ({mod}, Nsymb={nsymb}, pream={preamble}, tx={tx_ms}ms)")
    if tr.search_window_symb > 0:
        coverage = tr.search_window_symb / tr.buffer_nsymb * 100 if tr.buffer_nsymb > 0 else 0
        print(f"  Coarse search: {tr.search_window_symb} of {tr.buffer_nsymb} symbols ({coverage:.0f}% of buffer)")
    print(f"{'='*80}{ts_note}")

    ref_time = tr.cmd_turbo_up_time if tr.cmd_turbo_up_time >= 0 else (
        tr.cmd_tx_start if tr.cmd_tx_start >= 0 else 0)

    def delta(t):
        return fmt_delta(ref_time, t)

    # CMD TX
    print(f"  {fmt_time(tr.cmd_turbo_up_time):>14}  {delta(tr.cmd_turbo_up_time):>8}  CMD  TURBO UP {from_name}->{to_name}")
    if tr.cmd_tx_start >= 0:
        print(f"  {fmt_time(tr.cmd_tx_start):>14}  {delta(tr.cmd_tx_start):>8}  CMD  PTT ON (TX start)")
    if tr.cmd_tx_end >= 0:
        tx_dur = (tr.cmd_tx_end - tr.cmd_tx_start) * 1000 if tr.cmd_tx_start >= 0 else 0
        print(f"  {fmt_time(tr.cmd_tx_end):>14}  {delta(tr.cmd_tx_end):>8}  CMD  PTT OFF (TX done, {tx_dur:.0f}ms frame)")
    if tr.cmd_rx_enter >= 0:
        print(f"  {fmt_time(tr.cmd_rx_enter):>14}  {delta(tr.cmd_rx_enter):>8}  CMD  Entering RX (timeout={tr.cmd_recv_timeout}ms)")

    # RSP decode attempts
    for i, att in enumerate(tr.rsp_attempts):
        print(f"  {fmt_time(att.time):>14}  {delta(att.time):>8}  RSP  Buffer fill #{i} (nUnder={att.nUnder})")
        print(f"                            RSP    energy: {att.energy_chunks}")

        if att.coarse_pream >= 0:
            in_bounds = att.coarse_pream > att.bounds_lo and att.coarse_pream < att.bounds_hi
            print(f"                            RSP    coarse: pream={att.coarse_pream} metric={att.coarse_metric:.3f}"
                  f" bounds=[{att.bounds_lo},{att.bounds_hi}] {'PASS' if in_bounds else 'SKIP'}")

        if att.recovery_type:
            print(f"                            RSP    recovery: {att.recovery_type}"
                  f" symb={att.recovery_symb} metric={att.recovery_metric:.3f}"
                  f" energy={att.recovery_energy:.2e}")

        if att.mean_H >= 0:
            h_status = "OK" if att.mean_H > 0.3 else "SKIP-H"
            print(f"                            RSP    chan_est: mean_H={att.mean_H:.4f} ({h_status})")

        for trial in att.trials:
            print(f"                            RSP    trial {trial['trial']}: {trial['result']}"
                  f" delay={trial['delay']} iter={trial['iter']}")

        if att.result == "OK":
            print(f"                            RSP    >> DECODE OK: delay_symb={att.delay_symb}"
                  f" ftr={att.ftr} proc={att.proc_ms}ms")
        elif att.result == "FAIL":
            print(f"                            RSP    >> DECODE FAIL (proc={att.proc_ms}ms)")

    # RSP ACK
    if tr.rsp_ack_start >= 0:
        print(f"  {fmt_time(tr.rsp_ack_start):>14}  {delta(tr.rsp_ack_start):>8}  RSP  TX ACK pattern ({tr.rsp_ack_config})")
    if tr.rsp_ack_done >= 0:
        ack_dur = (tr.rsp_ack_done - tr.rsp_ack_start) * 1000 if tr.rsp_ack_start >= 0 else 0
        print(f"  {fmt_time(tr.rsp_ack_done):>14}  {delta(tr.rsp_ack_done):>8}  RSP  ACK done + flush ({ack_dur:.0f}ms)")

    # CMD ACK polls (show first, last, and detection)
    if tr.cmd_ack_polls:
        first = tr.cmd_ack_polls[0]
        last = tr.cmd_ack_polls[-1]
        peak = max(tr.cmd_ack_polls, key=lambda x: x[1])
        print(f"  {fmt_time(first[0]):>14}  {delta(first[0]):>8}  CMD  ACK poll start (matched={first[1]}/16)")
        if len(tr.cmd_ack_polls) > 2:
            print(f"                            CMD    ... {len(tr.cmd_ack_polls)} polls, peak matched={peak[1]}/16")
        print(f"  {fmt_time(last[0]):>14}  {delta(last[0]):>8}  CMD  ACK poll end (matched={last[1]}/16)")

    if tr.cmd_ack_detected >= 0:
        print(f"  {fmt_time(tr.cmd_ack_detected):>14}  {delta(tr.cmd_ack_detected):>8}  CMD  ACK DETECTED")

    # Summary deltas
    print(f"  {'-'*70}")
    if tr.cmd_tx_start >= 0 and tr.rsp_attempts:
        first_buf = tr.rsp_attempts[0].time
        print(f"  CMD TX start -> RSP first buffer:  {fmt_delta(tr.cmd_tx_start, first_buf)}")
    if tr.cmd_tx_end >= 0 and tr.rsp_attempts:
        first_buf = tr.rsp_attempts[0].time
        print(f"  CMD TX end   -> RSP first buffer:  {fmt_delta(tr.cmd_tx_end, first_buf)}")
    if tr.rsp_ack_done >= 0 and tr.cmd_ack_detected >= 0:
        print(f"  RSP ACK done -> CMD ACK detected:  {fmt_delta(tr.rsp_ack_done, tr.cmd_ack_detected)}")
    total_start = tr.cmd_turbo_up_time if tr.cmd_turbo_up_time >= 0 else tr.cmd_tx_start
    total_end = tr.cmd_ack_detected if tr.cmd_ack_detected >= 0 else (
        tr.cmd_timeout_time if tr.cmd_timeout_time >= 0 else -1)
    if total_start >= 0 and total_end >= 0:
        print(f"  Total transition time:            {fmt_delta(total_start, total_end)}")


def print_summary_table(transitions, has_timestamps):
    """Print a compact one-line-per-transition summary."""
    ts_note = "" if has_timestamps else "  (timestamps approximate)"
    print(f"\n{'='*110}")
    print(f"  TURBOSHIFT TRANSITION SUMMARY{ts_note}")
    print(f"{'='*110}")
    print(f"  {'#':>3} {'Dir':>4} {'From':>10} {'To':>10} {'Result':>6}"
          f" {'Attempts':>8} {'Coarse':>8} {'Search%':>7}"
          f" {'TX>Buf':>8} {'Total':>8} {'Notes'}")
    print(f"  {'-'*107}")

    for tr in transitions:
        from_name = config_name(tr.config_from)
        to_name = config_name(tr.config_to)
        result = "OK" if tr.success else "NAck" if tr.nack else "???"
        n_attempts = len(tr.rsp_attempts)

        # Best coarse metric across attempts
        best_coarse = max((a.coarse_metric for a in tr.rsp_attempts), default=0)

        # Search coverage
        coverage = ""
        if tr.search_window_symb > 0 and tr.buffer_nsymb > 0:
            pct = tr.search_window_symb / tr.buffer_nsymb * 100
            coverage = f"{pct:.0f}%"

        # TX -> first buffer delta
        tx_to_buf = ""
        if tr.cmd_tx_start >= 0 and tr.rsp_attempts:
            delta_ms = (tr.rsp_attempts[0].time - tr.cmd_tx_start) * 1000
            tx_to_buf = f"{delta_ms:.0f}ms"

        # Total time
        total = ""
        total_start = tr.cmd_turbo_up_time if tr.cmd_turbo_up_time >= 0 else tr.cmd_tx_start
        if tr.success and tr.cmd_ack_detected >= 0 and total_start >= 0:
            total = f"{(tr.cmd_ack_detected - total_start)*1000:.0f}ms"

        # Notes
        notes = []
        for att in tr.rsp_attempts:
            if att.recovery_type:
                notes.append(att.recovery_type)
            if att.coarse_metric == 0.0 and att.coarse_pream >= 0:
                notes.append("metric=0!")
        notes_str = ", ".join(set(notes))

        marker = " <<<" if tr.nack else ""
        print(f"  {tr.index:>3} {tr.direction[:3]:>4} {from_name:>10} {to_name:>10} {result:>6}"
              f" {n_attempts:>8} {best_coarse:>8.3f} {coverage:>7}"
              f" {tx_to_buf:>8} {total:>8} {notes_str}{marker}")

    # Totals
    n_ok = sum(1 for t in transitions if t.success)
    n_nack = sum(1 for t in transitions if t.nack)
    print(f"  {'-'*107}")
    print(f"  Total: {len(transitions)} transitions, {n_ok} OK, {n_nack} NAck")


def print_search_window_analysis(transitions):
    """Show coarse search window coverage analysis."""
    print(f"\n{'='*80}")
    print(f"  COARSE SEARCH WINDOW ANALYSIS")
    print(f"  (search window = 2*preamble_nSymb + Nsymb, should cover full buffer)")
    print(f"{'='*80}")
    print(f"  {'Config':>10} {'Mod':>6} {'pream':>5} {'Nsymb':>5} {'Window':>6} {'Buffer':>6} {'Cover%':>6} {'Status'}")
    print(f"  {'-'*60}")

    seen = set()
    for tr in transitions:
        cfg = tr.config_from
        if cfg in seen:
            continue
        seen.add(cfg)
        props = CONFIG_PROPS.get(cfg, {})
        if not props.get("nsymb"):
            continue
        nsymb = props["nsymb"]
        preamble = props["preamble"]
        window = 2 * preamble + nsymb
        # buffer_nsymb = max(frame*2, frame + turnaround_symb)
        frame = preamble + nsymb
        turnaround = 57  # ceil(1200/22.667) + 4
        buffer = max(frame * 2, frame + turnaround)
        coverage = window / buffer * 100

        status = "OK" if coverage > 80 else "NARROW" if coverage > 50 else "CRITICAL!"
        print(f"  {config_name(cfg):>10} {props['mod']:>6} {preamble:>5} {nsymb:>5}"
              f" {window:>6} {buffer:>6} {coverage:>5.0f}% {status}")

    print(f"\n  Configs with coverage < 50% will miss preambles arriving in the buffer's")
    print(f"  second half. This is the root cause of metric=0.000 at high configs.")


def main():
    parser = argparse.ArgumentParser(description="Mercury Turboshift Log Analyzer")
    parser.add_argument("logfile", help="Path to timestamped log file")
    parser.add_argument("--nacks", action="store_true", help="Only show NAcked transitions")
    parser.add_argument("--summary", action="store_true", help="Only show summary table")
    parser.add_argument("--transition", type=int, default=None, help="Show detail for transition N")
    parser.add_argument("--raw", action="store_true", help="Show all parsed events")
    parser.add_argument("--search-window", action="store_true", help="Analyze coarse search window coverage")
    parser.add_argument("--crash", action="store_true", help="Focus on crash detection analysis")
    args = parser.parse_args()

    print(f"Parsing {args.logfile}...")
    events, has_timestamps = parse_log(args.logfile)
    print(f"  {len(events)} events parsed (timestamps: {'precise' if has_timestamps else 'approximate/line-proxy'})")

    # Always run crash detection
    crashes = detect_crashes(events)
    if crashes:
        print(f"  ** {len(crashes)} CRASH(ES) DETECTED **")

    transitions = build_transitions(events)
    print(f"  {len(transitions)} SET_CONFIG transitions found")

    if args.crash:
        print_crash_report(crashes, events)
        return

    if args.raw:
        for evt in events:
            print(f"  {fmt_time(evt.time)} L{evt.line_num:>6} [{evt.role}] {evt.event_type} {evt.data}")
        return

    if args.search_window:
        print_search_window_analysis(transitions)
        return

    if args.summary:
        print_summary_table(transitions, has_timestamps)
        if crashes:
            print_crash_report(crashes, events)
        return

    if args.transition is not None:
        if 0 <= args.transition < len(transitions):
            print_transition_detail(transitions[args.transition], has_timestamps)
        else:
            print(f"Transition {args.transition} not found (0-{len(transitions)-1} available)")
        return

    # Default: summary + detail for NAcked transitions + crash report
    print_summary_table(transitions, has_timestamps)

    nacked = [t for t in transitions if t.nack]
    if nacked:
        print(f"\n\n{'#'*80}")
        print(f"  DETAILED TIMELINE FOR {len(nacked)} NAcked TRANSITION(S)")
        print(f"{'#'*80}")
        for tr in nacked:
            print_transition_detail(tr, has_timestamps)
    else:
        print("\n  No NAcked transitions found!")

    # Always show crash report
    print_crash_report(crashes, events)

    print_search_window_analysis(transitions)


if __name__ == "__main__":
    main()
