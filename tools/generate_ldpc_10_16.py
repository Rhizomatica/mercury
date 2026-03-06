#!/usr/bin/env python3
"""
Generate LDPC parity-check matrix for Mercury modem.
Rate 10/16: K=1000 info bits, P=600 parity bits, N=1600 total.

Uses Progressive Edge Growth (PEG) algorithm with IRA accumulator structure.
All math done computationally — no manual matrix construction.

Output: mercury_normal_10_16.h and mercury_normal_10_16.cc
"""

import numpy as np
import random
import sys
from collections import deque

# Fix seed for reproducibility
random.seed(42)
np.random.seed(42)

N = 1600   # codeword length
K = 1000   # information bits
P = N - K  # = 600 parity bits

print(f"Generating LDPC matrix: N={N}, K={K}, P={P}, rate={K/N:.4f}")

# ============================================================
# Step 1: Design degree distribution
# ============================================================
# For rate 10/16 (5/8) at N=1600:
# - Parity bits: degree 2 (IRA dual-diagonal), total parity edges = 1 + 599*2 = 1199
# - Info bits: mix of degree 3, 4, 6
#   600 bits at degree 3 = 1800 edges
#   300 bits at degree 4 = 1200 edges
#   100 bits at degree 6 = 600 edges
#   Total info edges = 3600
# - Total edges = 3600 + 1199 = 4799
# - Avg check degree = 4799 / 600 = 8.0

info_degrees = []
# 600 bits at degree 3
info_degrees.extend([3] * 600)
# 300 bits at degree 4
info_degrees.extend([4] * 300)
# 100 bits at degree 6
info_degrees.extend([6] * 100)

assert len(info_degrees) == K, f"Expected {K} info degrees, got {len(info_degrees)}"

total_info_edges = sum(info_degrees)
total_parity_edges = 1 + (P - 1) * 2  # first parity has degree 1, rest have degree 2
total_edges = total_info_edges + total_parity_edges
avg_check_deg = total_edges / P
print(f"Info edges: {total_info_edges}, Parity edges: {total_parity_edges}")
print(f"Total edges: {total_edges}, Avg check degree: {avg_check_deg:.1f}")

# ============================================================
# Step 2: Build IRA parity structure
# ============================================================
check_to_bits = [set() for _ in range(P)]
bit_to_checks = [set() for _ in range(N)]
check_degrees = np.zeros(P, dtype=int)

# IRA dual-diagonal for parity bits (columns K to N-1):
check_to_bits[0].add(K)
bit_to_checks[K].add(0)
check_degrees[0] += 1

for i in range(1, P):
    check_to_bits[i].add(K + i - 1)
    check_to_bits[i].add(K + i)
    bit_to_checks[K + i - 1].add(i)
    bit_to_checks[K + i].add(i)
    check_degrees[i] += 2

print(f"IRA structure added. Check degree range: {check_degrees.min()}-{check_degrees.max()}")

# ============================================================
# Step 3: PEG algorithm for info bit connections
# ============================================================
def peg_find_check(bit_j, check_to_bits, bit_to_checks, check_degrees, P):
    """
    PEG: Find the best check node to connect to bit_j.
    Uses BFS to find checks at maximum graph distance from bit_j.
    Among those, pick the one with minimum degree.
    """
    visited_checks = set()
    visited_bits = {bit_j}
    current_bits = {bit_j}
    best_candidates = None

    for depth in range(50):
        next_checks = set()
        for b in current_bits:
            for c in bit_to_checks[b]:
                if c not in visited_checks:
                    next_checks.add(c)
                    visited_checks.add(c)

        if not next_checks:
            break

        next_bits = set()
        for c in next_checks:
            for b in check_to_bits[c]:
                if b not in visited_bits:
                    next_bits.add(b)
                    visited_bits.add(b)

        current_bits = next_bits

        unvisited = set(range(P)) - visited_checks
        candidates = [c for c in unvisited if bit_j not in check_to_bits[c]]

        if candidates:
            best_candidates = candidates

    if best_candidates is None:
        all_candidates = [c for c in range(P) if bit_j not in check_to_bits[c]]
        if not all_candidates:
            return None
        best_candidates = all_candidates

    min_deg = min(check_degrees[c] for c in best_candidates)
    best = [c for c in best_candidates if check_degrees[c] == min_deg]
    return random.choice(best)


print("Running PEG algorithm for info bits...")
bit_order = list(range(K))
random.shuffle(bit_order)

progress_step = K // 10
for idx, j in enumerate(bit_order):
    if idx % progress_step == 0:
        print(f"  Progress: {idx}/{K} bits ({100*idx//K}%)")

    target_deg = info_degrees[j]
    for edge in range(target_deg):
        c = peg_find_check(j, check_to_bits, bit_to_checks, check_degrees, P)
        if c is None:
            print(f"  WARNING: bit {j} couldn't get edge {edge+1}/{target_deg}")
            break
        check_to_bits[c].add(j)
        bit_to_checks[j].add(c)
        check_degrees[c] += 1

print(f"PEG complete. Check degree range: {check_degrees.min()}-{check_degrees.max()}")
print(f"Check degree distribution:")
unique, counts = np.unique(check_degrees, return_counts=True)
for d, cnt in zip(unique, counts):
    print(f"  degree {d}: {cnt} checks")

# ============================================================
# Step 4: Verify matrix properties
# ============================================================
print("\nVerifying matrix properties...")

for j in range(K):
    actual_deg = len(bit_to_checks[j])
    expected_deg = info_degrees[j]
    if actual_deg != expected_deg:
        print(f"  ERROR: info bit {j} has degree {actual_deg}, expected {expected_deg}")

for j in range(K, N):
    actual_deg = len(bit_to_checks[j])
    expected_deg = 1 if j == K else 2
    if actual_deg != expected_deg:
        print(f"  ERROR: parity bit {j} has degree {actual_deg}, expected {expected_deg}")

def compute_girth_sample(check_to_bits, bit_to_checks, P, N, sample_size=200):
    """Estimate girth by sampling random bits and doing BFS."""
    min_girth = float('inf')
    sample_bits = random.sample(range(N), min(sample_size, N))

    for start_bit in sample_bits:
        visited = {}
        visited[start_bit] = 0
        queue = deque()

        for c in bit_to_checks[start_bit]:
            for b in check_to_bits[c]:
                if b == start_bit:
                    continue
                if b in visited:
                    continue
                visited[b] = 1
                queue.append((b, 1, c))

        while queue:
            bit, depth, from_check = queue.popleft()
            if depth >= min_girth // 2:
                break

            for c in bit_to_checks[bit]:
                if c == from_check:
                    continue
                for b in check_to_bits[c]:
                    if b == bit:
                        continue
                    if b == start_bit:
                        cycle_len = 2 * (depth + 1)
                        min_girth = min(min_girth, cycle_len)
                        break
                    if b not in visited:
                        visited[b] = depth + 1
                        queue.append((b, depth + 1, c))

    return min_girth

girth = compute_girth_sample(check_to_bits, bit_to_checks, P, N)
print(f"  Estimated girth (shortest cycle): {girth}")
if girth < 4:
    print("  WARNING: Girth < 4! Matrix may have poor performance.")
elif girth >= 6:
    print("  Good: girth >= 6")

# Check 4: Verify encoding works
print("\nVerifying encoding...")

H = np.zeros((P, N), dtype=np.int8)
for i in range(P):
    for j in check_to_bits[i]:
        H[i, j] = 1

test_msg = np.random.randint(0, 2, K)
encoded = np.zeros(N, dtype=np.int8)
encoded[:K] = test_msg

for i in range(P):
    val = 0
    for j in check_to_bits[i]:
        if j != K + i:
            val ^= encoded[j]
    encoded[K + i] = val

syndrome = H @ encoded % 2
if np.all(syndrome == 0):
    print("  Encoding verification PASSED (syndrome = 0)")
else:
    nonzero = np.sum(syndrome != 0)
    print(f"  Encoding verification FAILED! {nonzero}/{P} non-zero syndrome bits")
    for i in range(min(5, P)):
        if syndrome[i] != 0:
            bits = sorted(check_to_bits[i])
            vals = [encoded[b] for b in bits]
            print(f"    Check {i}: bits={bits}, vals={vals}, XOR={sum(vals)%2}")

all_pass = True
for trial in range(10):
    test_msg = np.random.randint(0, 2, K)
    encoded[:K] = test_msg
    for i in range(P):
        val = 0
        for j in check_to_bits[i]:
            if j != K + i:
                val ^= encoded[j]
        encoded[K + i] = val
    syndrome = H @ encoded % 2
    if not np.all(syndrome == 0):
        print(f"  Trial {trial}: FAILED")
        all_pass = False

if all_pass:
    print(f"  All 10 additional encoding trials PASSED")

# ============================================================
# Step 5: Simple BER simulation (AWGN, BPSK)
# ============================================================
print("\nRunning BER simulation (BPSK/AWGN, 50 iterations SPA)...")

def spa_decode(llr_input, check_to_bits, bit_to_checks, P, N, max_iter=50):
    """Sum-Product Algorithm decoder."""
    L = llr_input.copy().astype(np.float64)

    R = {}
    for i in range(P):
        for j in check_to_bits[i]:
            R[(i, j)] = 0.0

    for iteration in range(max_iter):
        for i in range(P):
            bits = list(check_to_bits[i])
            if len(bits) < 2:
                continue

            Q_vals = []
            for j in bits:
                q = L[j]
                for i2 in bit_to_checks[j]:
                    if i2 != i:
                        q += R.get((i2, j), 0.0)
                Q_vals.append(q)

            for idx, j in enumerate(bits):
                prod_tanh = 1.0
                for idx2, j2 in enumerate(bits):
                    if idx2 != idx:
                        val = Q_vals[idx2] / 2.0
                        val = np.clip(val, -19.0, 19.0)
                        prod_tanh *= np.tanh(val)
                prod_tanh = np.clip(prod_tanh, -0.9999999, 0.9999999)
                R[(i, j)] = 2.0 * np.arctanh(prod_tanh)

        L_total = llr_input.copy().astype(np.float64)
        for j in range(N):
            for i in bit_to_checks[j]:
                L_total[j] += R.get((i, j), 0.0)

        hard = (L_total < 0).astype(np.int8)

        syndrome = np.zeros(P, dtype=np.int8)
        for i in range(P):
            s = 0
            for j in check_to_bits[i]:
                s ^= hard[j]
            syndrome[i] = s

        if np.all(syndrome == 0):
            return hard, True, iteration + 1

    return hard, False, max_iter


snr_dbs = [1.0, 2.0, 3.0, 4.0, 5.0]
n_frames_per_snr = 20

for snr_db in snr_dbs:
    snr_lin = 10 ** (snr_db / 10)
    noise_var = 1.0 / snr_lin
    noise_std = np.sqrt(noise_var)

    bit_errors = 0
    frame_errors = 0
    total_bits = 0
    decode_failures = 0

    for frame in range(n_frames_per_snr):
        msg = np.random.randint(0, 2, K)

        codeword = np.zeros(N, dtype=np.int8)
        codeword[:K] = msg
        for i in range(P):
            val = 0
            for j in check_to_bits[i]:
                if j != K + i:
                    val ^= codeword[j]
            codeword[K + i] = val

        tx = 1.0 - 2.0 * codeword
        noise = noise_std * np.random.randn(N)
        rx = tx + noise
        llr = 2.0 * rx / noise_var

        decoded, success, iters = spa_decode(llr, check_to_bits, bit_to_checks, P, N)

        if not success:
            decode_failures += 1
            frame_errors += 1
        else:
            errors = np.sum(decoded[:K] != msg)
            bit_errors += errors
            if errors > 0:
                frame_errors += 1

        total_bits += K

    ber = bit_errors / total_bits if total_bits > 0 else 0
    fer = frame_errors / n_frames_per_snr
    print(f"  SNR={snr_db:.1f}dB: BER={ber:.6f}, FER={fer:.2f}, "
          f"decode_fail={decode_failures}/{n_frames_per_snr}")

# ============================================================
# Step 6: Extract Mercury-format arrays
# ============================================================
print("\nExtracting Mercury-format arrays...")

Cwidth = max(len(check_to_bits[i]) for i in range(P))
print(f"  Cwidth = {Cwidth}")

QCmatrixC = np.full((P, Cwidth), -1, dtype=np.int32)
for i in range(P):
    bits = sorted(check_to_bits[i])
    for idx, b in enumerate(bits):
        QCmatrixC[i, idx] = b

Vwidth = max(len(bit_to_checks[j]) for j in range(N))
print(f"  Vwidth = {Vwidth}")

QCmatrixV = np.full((N, Vwidth), -1, dtype=np.int32)
for j in range(N):
    checks = sorted(bit_to_checks[j])
    for idx, c in enumerate(checks):
        QCmatrixV[j, idx] = c

EncWidth = Cwidth - 1
QCmatrixEnc = np.full((P, EncWidth), -1, dtype=np.int32)
for i in range(P):
    bits = sorted(check_to_bits[i])
    enc_bits = [b for b in bits if b != K + i]
    for idx, b in enumerate(enc_bits):
        if idx >= EncWidth:
            print(f"  WARNING: check {i} has more non-self bits than EncWidth={EncWidth}")
            break
        QCmatrixEnc[i, idx] = b

print("  Computing degree distribution...")
bit_degrees = [len(bit_to_checks[j]) for j in range(N)]

d_pairs = []
current_deg = bit_degrees[0]
current_count = 1
for j in range(1, N):
    if bit_degrees[j] == current_deg:
        current_count += 1
    else:
        d_pairs.append((current_count, current_deg))
        current_deg = bit_degrees[j]
        current_count = 1
d_pairs.append((current_count, current_deg))

dwidth = len(d_pairs) * 2
print(f"  dwidth = {dwidth} ({len(d_pairs)} groups)")
for count, deg in d_pairs:
    print(f"    {count} bits at degree {deg}")

# ============================================================
# Step 7: Write C header file
# ============================================================
print("\nWriting mercury_normal_10_16.h ...")

header_path = "../include/physical_layer/mercury_normal_10_16.h"
with open(header_path, 'w') as f:
    f.write("/*\n")
    f.write(" * LDPC parity-check matrix for Mercury modem\n")
    f.write(f" * Rate 10/16 (K={K}, P={P}, N={N})\n")
    f.write(" * Generated by tools/generate_ldpc_10_16.py\n")
    f.write(" * PEG algorithm with IRA accumulator structure\n")
    f.write(" */\n\n")
    f.write("#ifndef MERCURY_NORMAL_10_16_H\n")
    f.write("#define MERCURY_NORMAL_10_16_H\n\n")
    f.write(f"extern int mercury_normal_Cwidth_10_16;\n")
    f.write(f"extern int mercury_normal_Vwidth_10_16;\n")
    f.write(f"extern int mercury_normal_dwidth_10_16;\n\n")
    f.write(f"extern int mercury_normal_QCmatrixC_10_16[{P}][{Cwidth}];\n")
    f.write(f"extern int mercury_normal_QCmatrixEnc_10_16[{P}][{EncWidth}];\n")
    f.write(f"extern int mercury_normal_QCmatrixV_10_16[{N}][{Vwidth}];\n")
    f.write(f"extern int mercury_normal_QCmatrixd_10_16[{dwidth}];\n\n")
    f.write("#endif\n")

print(f"  Written: {header_path}")

# ============================================================
# Step 8: Write C source file
# ============================================================
print("Writing mercury_normal_10_16.cc ...")

source_path = "../source/physical_layer/mercury_normal_10_16.cc"
with open(source_path, 'w') as f:
    f.write("/*\n")
    f.write(" * LDPC parity-check matrix for Mercury modem\n")
    f.write(f" * Rate 10/16 (K={K}, P={P}, N={N})\n")
    f.write(" * Generated by tools/generate_ldpc_10_16.py\n")
    f.write(" * PEG algorithm with IRA accumulator structure\n")
    f.write(" */\n\n")
    f.write('#include "physical_layer/mercury_normal_10_16.h"\n\n')

    f.write(f"int mercury_normal_Cwidth_10_16 = {Cwidth};\n")
    f.write(f"int mercury_normal_Vwidth_10_16 = {Vwidth};\n")
    f.write(f"int mercury_normal_dwidth_10_16 = {dwidth};\n\n")

    # QCmatrixC
    f.write(f"int mercury_normal_QCmatrixC_10_16[{P}][{Cwidth}] = {{\n")
    for i in range(P):
        row = ','.join(str(QCmatrixC[i, j]) for j in range(Cwidth))
        comma = ',' if i < P - 1 else ''
        f.write(f"{{{row}}}{comma}\n")
    f.write("};\n\n")

    # QCmatrixEnc
    f.write(f"int mercury_normal_QCmatrixEnc_10_16[{P}][{EncWidth}] = {{\n")
    for i in range(P):
        row = ','.join(str(QCmatrixEnc[i, j]) for j in range(EncWidth))
        comma = ',' if i < P - 1 else ''
        f.write(f"{{{row}}}{comma}\n")
    f.write("};\n\n")

    # QCmatrixV
    f.write(f"int mercury_normal_QCmatrixV_10_16[{N}][{Vwidth}] = {{\n")
    for j in range(N):
        row = ','.join(str(QCmatrixV[j, k]) for k in range(Vwidth))
        comma = ',' if j < N - 1 else ''
        f.write(f"{{{row}}}{comma}\n")
    f.write("};\n\n")

    # QCmatrixd
    f.write(f"int mercury_normal_QCmatrixd_10_16[{dwidth}] = {{")
    d_flat = []
    for count, deg in d_pairs:
        d_flat.extend([count, deg])
    f.write(','.join(str(x) for x in d_flat))
    f.write("};\n")

print(f"  Written: {source_path}")
print("\nDone! Verify by building and running BER test.")
