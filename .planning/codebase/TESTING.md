# Testing — Mercury (HERMES HF Modem)

## Test Infrastructure

Mercury has **minimal formal testing infrastructure**. Testing is primarily done through diagnostic utilities and manual radio testing.

## Built-in Test Modes

Mercury itself supports TX/RX test modes via CLI flags:

```bash
# TX test mode — transmits test pattern
./mercury -t -m [mode_index] -i [device] -o [device]

# RX test mode — receives and validates test pattern
./mercury -r -m [mode_index] -i [device] -o [device]
```

These are implemented in `modem/modem.c` (`run_tests_tx`, `run_tests_rx`).

## Diagnostic Utilities (`utils/`)

| Utility | File | Purpose |
|---------|------|---------|
| `broadcast_diag_rx` | `utils/broadcast_diag_rx.c` | Receive and diagnose broadcast frames |
| `broadcast_diag_tx` | `utils/broadcast_diag_tx.c` | Transmit broadcast test frames |
| `broadcast_test` | `utils/broadcast_test.c` | End-to-end broadcast test harness |

Built via `make -C utils`.

## FreeDV Unit Tests

The vendored FreeDV library includes its own test suite:

- `modem/freedv/unittest/` — FreeDV unit tests
- `modem/freedv/unittest/raw_data_curves/` — performance curves with Makefile
- Standalone test binaries: `freedv_data_raw_rx`, `freedv_data_raw_tx`, `freedv_data_rx`, `freedv_data_tx`

## mercury-qt Test Support

The companion GUI has a basic test class:

- `mercury-qt/core/test_class.py` — `TestClass` with `start_mercury_qt_app()` method
- Invoked via `python app.py test`

## No Formal Test Framework

- No unit test framework (no CUnit, Check, Unity, etc.)
- No CI/CD pipeline visible in the repository
- No automated integration tests
- Testing relies on real hardware (radio + sound card) or loopback audio
- The `docs/ARQ.md` mentions OTA (over-the-air) tuning and testing procedures

## Coverage

No code coverage tooling configured.

## What Exists vs What's Missing

| Aspect | Status |
|--------|--------|
| Hardware TX/RX test modes | ✓ Built into mercury binary |
| Broadcast diagnostic tools | ✓ In `utils/` |
| FreeDV modem unit tests | ✓ In vendored library |
| ARQ protocol unit tests | ✗ None |
| TCP TNC interface tests | ✗ None |
| WebSocket/GUI tests | ✗ None |
| Integration tests | ✗ None (manual radio testing) |
| CI/CD | ✗ None visible |
