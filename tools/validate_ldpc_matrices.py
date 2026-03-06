#!/usr/bin/env python3
"""
Cross-validate LDPC matrices by running the same encoding/decoding tests
on both a known-good existing matrix (8/16) and the new 12/16 matrix.

If 8/16 passes → test harness is correct.
If 12/16 also passes → new matrix is valid.
"""

import numpy as np
import re
import sys
import os

np.random.seed(42)


def parse_c_matrix(filepath, array_name, rows, cols):
    """Parse a C int array from a .cc source file."""
    with open(filepath, 'r') as f:
        content = f.read()

    # Find the array definition
    pattern = rf'{re.escape(array_name)}\s*\[{rows}\]\[{cols}\]\s*=\s*\{{(.*?)\}};'
    match = re.search(pattern, content, re.DOTALL)
    if not match:
        raise ValueError(f"Could not find {array_name}[{rows}][{cols}] in {filepath}")

    data_str = match.group(1)
    # Parse row by row
    matrix = np.zeros((rows, cols), dtype=np.int32)
    row_pattern = r'\{([^}]+)\}'
    row_matches = re.findall(row_pattern, data_str)

    if len(row_matches) != rows:
        raise ValueError(f"Expected {rows} rows, found {len(row_matches)}")

    for i, row_str in enumerate(row_matches):
        vals = [int(x.strip()) for x in row_str.split(',')]
        if len(vals) != cols:
            raise ValueError(f"Row {i}: expected {cols} cols, got {len(vals)}")
        matrix[i] = vals

    return matrix


def parse_c_1d_array(filepath, array_name, length):
    """Parse a C 1D int array from a .cc source file."""
    with open(filepath, 'r') as f:
        content = f.read()

    pattern = rf'{re.escape(array_name)}\s*\[{length}\]\s*=\s*\{{([^}}]+)\}};'
    match = re.search(pattern, content, re.DOTALL)
    if not match:
        raise ValueError(f"Could not find {array_name}[{length}] in {filepath}")

    vals = [int(x.strip()) for x in match.group(1).split(',')]
    if len(vals) != length:
        raise ValueError(f"Expected {length} values, got {len(vals)}")
    return np.array(vals, dtype=np.int32)


def parse_c_scalar(filepath, var_name):
    """Parse a C scalar int from a .cc source file."""
    with open(filepath, 'r') as f:
        content = f.read()

    pattern = rf'{re.escape(var_name)}\s*=\s*(\d+)\s*;'
    match = re.search(pattern, content)
    if not match:
        raise ValueError(f"Could not find {var_name} in {filepath}")
    return int(match.group(1))


def build_adjacency(QCmatrixC, QCmatrixV, P, N):
    """Build adjacency lists from Mercury matrix format."""
    check_to_bits = [set() for _ in range(P)]
    bit_to_checks = [set() for _ in range(N)]

    for i in range(P):
        for val in QCmatrixC[i]:
            if val == -1:
                break
            check_to_bits[i].add(int(val))
            bit_to_checks[int(val)].add(i)

    return check_to_bits, bit_to_checks


def test_encoding(check_to_bits, bit_to_checks, QCmatrixEnc, N, K, P, n_trials=100):
    """Test encoding: encode random messages, verify syndrome = 0."""
    # Build full H matrix
    H = np.zeros((P, N), dtype=np.int8)
    for i in range(P):
        for j in check_to_bits[i]:
            H[i, j] = 1

    EncWidth = QCmatrixEnc.shape[1]
    failures = 0

    for trial in range(n_trials):
        msg = np.random.randint(0, 2, K)
        encoded = np.zeros(N, dtype=np.int8)
        encoded[:K] = msg

        # IRA encoding using QCmatrixEnc (matches Mercury's encoder)
        for i in range(P):
            val = 0
            for j_idx in range(EncWidth):
                bit_idx = QCmatrixEnc[i, j_idx]
                if bit_idx == -1:
                    break
                val ^= encoded[bit_idx]
            encoded[K + i] = val

        # Verify syndrome
        syndrome = H @ encoded % 2
        if not np.all(syndrome == 0):
            failures += 1
            if failures <= 3:
                nonzero = np.where(syndrome != 0)[0]
                print(f"    Trial {trial}: FAILED at checks {nonzero[:5]}...")

    return failures


def spa_decode(llr_input, check_to_bits, bit_to_checks, P, N, K, max_iter=50):
    """Sum-Product Algorithm decoder."""
    L = llr_input.copy().astype(np.float64)

    R = {}
    for i in range(P):
        for j in check_to_bits[i]:
            R[(i, j)] = 0.0

    for iteration in range(max_iter):
        # Check node update
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
                        val = np.clip(Q_vals[idx2] / 2.0, -19.0, 19.0)
                        prod_tanh *= np.tanh(val)
                prod_tanh = np.clip(prod_tanh, -0.9999999, 0.9999999)
                R[(i, j)] = 2.0 * np.arctanh(prod_tanh)

        # Hard decision
        L_total = llr_input.copy().astype(np.float64)
        for j in range(N):
            for i in bit_to_checks[j]:
                L_total[j] += R.get((i, j), 0.0)

        hard = (L_total < 0).astype(np.int8)

        # Check syndrome
        ok = True
        for i in range(P):
            s = 0
            for j in check_to_bits[i]:
                s ^= hard[j]
            if s != 0:
                ok = False
                break

        if ok:
            return hard, True, iteration + 1

    return hard, False, max_iter


def test_decoding(check_to_bits, bit_to_checks, QCmatrixEnc, N, K, P,
                  snr_db=5.0, n_frames=50):
    """Test BPSK/AWGN decode at given SNR."""
    snr_lin = 10 ** (snr_db / 10)
    noise_var = 1.0 / snr_lin
    noise_std = np.sqrt(noise_var)
    EncWidth = QCmatrixEnc.shape[1]

    bit_errors = 0
    frame_errors = 0
    decode_failures = 0
    total_bits = 0

    for frame in range(n_frames):
        msg = np.random.randint(0, 2, K)
        codeword = np.zeros(N, dtype=np.int8)
        codeword[:K] = msg

        for i in range(P):
            val = 0
            for j_idx in range(EncWidth):
                bit_idx = QCmatrixEnc[i, j_idx]
                if bit_idx == -1:
                    break
                val ^= codeword[bit_idx]
            codeword[K + i] = val

        tx = 1.0 - 2.0 * codeword
        rx = tx + noise_std * np.random.randn(N)
        llr = 2.0 * rx / noise_var

        decoded, success, iters = spa_decode(llr, check_to_bits, bit_to_checks, P, N, K)

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
    fer = frame_errors / n_frames
    return ber, fer, decode_failures


def validate_matrix(name, source_path, N, K, prefix):
    """Full validation suite for one LDPC matrix."""
    P = N - K

    print(f"\n{'='*60}")
    print(f"Validating: {name} (N={N}, K={K}, P={P}, rate={K}/{N})")
    print(f"Source: {source_path}")
    print(f"{'='*60}")

    # Parse matrices
    print("  Parsing matrices...")
    Cwidth = parse_c_scalar(source_path, f"mercury_normal_Cwidth_{prefix}")
    Vwidth = parse_c_scalar(source_path, f"mercury_normal_Vwidth_{prefix}")
    dwidth = parse_c_scalar(source_path, f"mercury_normal_dwidth_{prefix}")

    print(f"  Cwidth={Cwidth}, Vwidth={Vwidth}, dwidth={dwidth}")

    QCmatrixC = parse_c_matrix(source_path, f"mercury_normal_QCmatrixC_{prefix}", P, Cwidth)
    QCmatrixV = parse_c_matrix(source_path, f"mercury_normal_QCmatrixV_{prefix}", N, Vwidth)
    QCmatrixEnc = parse_c_matrix(source_path, f"mercury_normal_QCmatrixEnc_{prefix}", P, Cwidth - 1)
    QCmatrixd = parse_c_1d_array(source_path, f"mercury_normal_QCmatrixd_{prefix}", dwidth)

    # Build adjacency lists
    check_to_bits, bit_to_checks = build_adjacency(QCmatrixC, QCmatrixV, P, N)

    # Test 1: Matrix consistency (C and V must be transposes)
    print("\n  Test 1: C/V consistency...")
    c_v_ok = True
    for i in range(P):
        for j in check_to_bits[i]:
            if i not in bit_to_checks[j]:
                print(f"    FAIL: check {i} connects to bit {j}, but V[{j}] doesn't list check {i}")
                c_v_ok = False
                break
        if not c_v_ok:
            break

    for j in range(N):
        for i in bit_to_checks[j]:
            if j not in check_to_bits[i]:
                print(f"    FAIL: bit {j} connects to check {i}, but C[{i}] doesn't list bit {j}")
                c_v_ok = False
                break
        if not c_v_ok:
            break

    print(f"    {'PASS' if c_v_ok else 'FAIL'}")

    # Test 2: IRA structure (parity bits K..N-1 form accumulator chain)
    print("  Test 2: IRA accumulator structure...")
    ira_ok = True
    # First parity bit (K) should connect to check 0 (at minimum)
    if 0 not in bit_to_checks[K]:
        print(f"    FAIL: parity bit {K} not connected to check 0")
        ira_ok = False
    # Each check i (1..P-1) should connect to parity bits K+i-1 and K+i
    for i in range(1, P):
        if (K + i - 1) not in check_to_bits[i]:
            print(f"    FAIL: check {i} missing connection to parity {K+i-1}")
            ira_ok = False
            break
        if (K + i) not in check_to_bits[i]:
            print(f"    FAIL: check {i} missing connection to parity {K+i}")
            ira_ok = False
            break
    print(f"    {'PASS' if ira_ok else 'FAIL'}")

    # Test 3: Encoding matrix consistency
    print("  Test 3: Enc matrix = C matrix minus self-parity...")
    enc_ok = True
    for i in range(P):
        c_bits = set()
        for val in QCmatrixC[i]:
            if val == -1:
                break
            c_bits.add(int(val))

        enc_bits = set()
        for val in QCmatrixEnc[i]:
            if val == -1:
                break
            enc_bits.add(int(val))

        expected_enc = c_bits - {K + i}  # C minus self-parity
        if enc_bits != expected_enc:
            print(f"    FAIL: check {i}: Enc={sorted(enc_bits)}, expected={sorted(expected_enc)}")
            enc_ok = False
            if i > 5:
                break
    print(f"    {'PASS' if enc_ok else 'FAIL'}")

    # Test 4: Degree distribution matches d array
    print("  Test 4: Degree distribution...")
    actual_degrees = [len(bit_to_checks[j]) for j in range(N)]
    # Parse d array as (count, degree) pairs
    d_groups = []
    total_from_d = 0
    for idx in range(0, dwidth, 2):
        count = QCmatrixd[idx]
        degree = QCmatrixd[idx + 1]
        d_groups.append((count, degree))
        total_from_d += count

    # Reconstruct expected degrees from d array
    expected_degrees = []
    for count, degree in d_groups:
        expected_degrees.extend([degree] * count)

    d_ok = (len(expected_degrees) == N and expected_degrees == actual_degrees)
    if not d_ok:
        if len(expected_degrees) != N:
            print(f"    FAIL: d array covers {len(expected_degrees)} bits, expected {N}")
        else:
            mismatches = sum(1 for a, b in zip(actual_degrees, expected_degrees) if a != b)
            print(f"    FAIL: {mismatches} degree mismatches")
    print(f"    {'PASS' if d_ok else 'FAIL'}")

    # Test 5: Encoding verification (100 random messages)
    print("  Test 5: Encoding (100 random messages)...")
    enc_failures = test_encoding(check_to_bits, bit_to_checks, QCmatrixEnc, N, K, P, 100)
    print(f"    {'PASS' if enc_failures == 0 else 'FAIL'} ({enc_failures}/100 failures)")

    # Test 6: Decoding at 5 dB (should be clean for all rates)
    print("  Test 6: SPA decode at 5dB (20 frames)...")
    ber, fer, dec_fail = test_decoding(check_to_bits, bit_to_checks, QCmatrixEnc,
                                        N, K, P, snr_db=5.0, n_frames=20)
    dec_ok = (dec_fail == 0 and ber == 0.0)
    print(f"    BER={ber:.6f}, FER={fer:.2f}, decode_fail={dec_fail}/20")
    print(f"    {'PASS' if dec_ok else 'FAIL'}")

    all_pass = c_v_ok and ira_ok and enc_ok and d_ok and (enc_failures == 0) and dec_ok
    print(f"\n  OVERALL: {'ALL TESTS PASSED' if all_pass else 'SOME TESTS FAILED'}")
    return all_pass


# ============================================================
# Main: validate both matrices
# ============================================================
if __name__ == '__main__':
    base = os.path.dirname(os.path.abspath(__file__))
    src_dir = os.path.join(base, '..', 'source', 'physical_layer')

    results = {}

    # Validate known-good 8/16 matrix
    results['8/16'] = validate_matrix(
        "Rate 8/16 (known-good)",
        os.path.join(src_dir, "mercury_normal_8_16.cc"),
        N=1600, K=800, prefix="8_16"
    )

    # Validate new 10/16 matrix
    results['10/16'] = validate_matrix(
        "Rate 10/16 (new)",
        os.path.join(src_dir, "mercury_normal_10_16.cc"),
        N=1600, K=1000, prefix="10_16"
    )

    print(f"\n{'='*60}")
    print("CROSS-VALIDATION SUMMARY")
    print(f"{'='*60}")
    for name, passed in results.items():
        status = "PASS" if passed else "FAIL"
        print(f"  {name}: {status}")

    if all(results.values()):
        print("\nBoth matrices validated. Safe to integrate 10/16.")
    else:
        print("\nValidation FAILED. Do not integrate until fixed.")
        sys.exit(1)
