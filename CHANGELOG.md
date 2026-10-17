# Changelog

All notable changes to this project are documented here. Versioning and the meaning of
contract and ABI changes are described in the README, section "Contract and versioning".

## 0.2.0 (2026-10-17)

- Transmitter after fldigi 4.2.13 shaped RTTY TX (`send_char`, `send_symbol`, `send_stop`,
  `send_idle`, `flush_stream`, `Oscillator`, `SymbolShaper`, `rcos`): additive
  `frtty_tx_*` API in `src/rtty_tx.c`, 5-bit codes in, real audio out, 8000/12000/48000 Hz,
  1/1.5/2 stop bits, per-instance `maxamp`.
- Version string carries the capability tag `tx=1`; contract `rtty-c1` and ABI 1
  unchanged, receiver unchanged (RX goldens identical).
- TX goldens: fldigi 4.2.13's own TX output for 45.45 Bd with 1, 1.5 and 2 stop bits and
  a long 50 Bd text; the port reproduces all four with 0 LSB difference.
- `make sanitize` (ASan + UBSan) target, run in CI.

## 0.1.1 (2026-10-16)

Documentation release; no change to the contract (`rtty-c1`), the ABI (1) or the
decoded output.

- README rewritten: scope, signal chain with fldigi 4.2.13 citations, configuration and
  API reference, C and Python usage, contract and versioning policy, verification,
  performance and memory, building, maintenance, roadmap.
- Comments in `src/rtty_rx.c` and `include/fldigi_rtty.h` now refer to the Python
  reference as the former one (wave-owl commit `b6be59b`), since it has been removed from
  wave-owl.
- Added this changelog.

- `frtty_version()` reports 0.1.1 and derives `abi=` from `FRTTY_ABI`.

## 0.1.0 (2026-10-16)

First release, contract `rtty-c1`, ABI 1.

- C99 port of wave-owl's Python port of the fldigi 4.2.13 RTTY receiver: mark/space
  mixers, raised-cosine RTTY filter (overlap-save FFT), envelope and noise trackers,
  optimal ATC, start-bit framing, optional AFC, SNR estimate.
- Codes identical to the Python reference on the golden data in `tests/golden/`; filter
  taps within 1e-12.
- Plain Makefile (static and shared library, out-of-tree builds), C test suite, CI on
  Ubuntu and macOS.
