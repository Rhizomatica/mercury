# Fuzzing

Mercury ships libFuzzer harnesses for its untrusted-byte parsers under
[`tests/fuzz/`](../tests/fuzz/README.md): the ARQ frame-header decoder, the modem
framer-byte parser, the KISS deframer, and the arithmetic decoder.

They are opt-in (require clang with libFuzzer) and are not part of the blocking CI
gates. See `tests/fuzz/README.md` for build, run, and crash-triage instructions.
