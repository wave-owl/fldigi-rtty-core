# fldigi-rtty-core

[![CI](https://github.com/wave-owl/fldigi-rtty-core/actions/workflows/ci.yml/badge.svg)](https://github.com/wave-owl/fldigi-rtty-core/actions/workflows/ci.yml)

A C99 library that receives RTTY (2-FSK teleprinter signals) the way fldigi 4.2.13 does:
audio samples in, 5-bit Baudot codes out. It is a C port of the Python port of the fldigi
receiver that [wave-owl](https://github.com/wave-owl/wave-owl) used before, and it is now
wave-owl's only RTTY receiver.

| | |
|---|---|
| **Is** | A single-channel 2-FSK RTTY **receiver**: one instance decodes one signal with known mark and space tones. Input is a stream of `double` audio samples, output is a stream of character codes (5 bits for Baudot, up to 8 bits configurable), plus an SNR estimate and the AFC offset. |
| **Is not** | A transmitter (no modulator yet; planned, see [Roadmap](#roadmap)). |
| | A Baudot-to-text converter: LTRS/FIGS shift handling, the figures table and unshift-on-space are left to the caller (wave-owl does this in `src/modes/rtty/ita2.py`). |
| | A skimmer: no multi-channel operation, no signal detection, no tone search. The caller tells each instance where mark and space are. |
| | An I/O layer: no sound card, file or network access. |
| | Threaded or allocating at run time: the library creates no threads and allocates memory only in `frtty_rx_create()` (the receive path `frtty_rx_process()` performs no allocation). It has no global or static mutable state. |

## Contents

- [Signal chain](#signal-chain)
- [Configuration reference](#configuration-reference)
- [API reference](#api-reference)
- [Usage examples](#usage-examples)
- [Contract and versioning](#contract-and-versioning)
- [Verification](#verification)
- [Performance and memory](#performance-and-memory)
- [Building](#building)
- [Maintenance and fldigi updates](#maintenance-and-fldigi-updates)
- [Roadmap](#roadmap)
- [License and credits](#license-and-credits)
- [Changelog](#changelog)

## Signal chain

All fldigi citations refer to fldigi tag `v4.2.13` (see [Provenance](#provenance)); all
C functions are in `src/rtty_rx.c`.

```
 audio x[n] (double)
      |
      |  frtty_rx_process(): split into blocks of one symbol (symbollen samples)
      v
 +----------------------------+        +----------------------------+
 | mark mixer                 |        | space mixer                |
 | x * e^{j phi_m},           |        | x * e^{j phi_s},           |
 | phi_m -= 2pi(fm+afc)/fs    |        | phi_s -= 2pi(fs_+afc)/fs   |
 +-------------+--------------+        +-------------+--------------+
               v                                     v
 +----------------------------+        +----------------------------+
 | raised-cosine RTTY filter  |        | raised-cosine RTTY filter  |
 | 256 taps, overlap-save FFT |        | 256 taps, overlap-save FFT |
 +-------------+--------------+        +-------------+--------------+
               v  |zm|                               v  |zs|
 +-------------------------------------------------------------------+
 | envelope (attack symbollen/4, decay symbollen*16) and noise floor |
 | (attack symbollen/4, decay symbollen*48) per tone; clip to [floor, env]
 +-------------------------------------------------------------------+
               v
 +-------------------------------------------------------------------+
 | optimal ATC:  bit = (mc-fl)(me-fl) - (sc-fl)(se-fl)                |
 |                     - 0.25((me-fl)^2 - (se-fl)^2)  > 0             |
 +-------------------------------------------------------------------+
               v  one bit per sample
 +-------------------------------------------------------------------+
 | symbol-long bit buffer -> framing state machine                   |
 | IDLE -> START (mark/space straddle) -> DATA (nbits, LSB first)     |
 |      -> STOP (must be mark) -> code                                |
 +-------------------------------------------------------------------+
        |                                   |
        v                                   v
   codes (uint8_t)        at each code: SNR accumulators, optional AFC
                          (offset applied to both mixers from the next block)
```

### 1. Blocking

`frtty_rx_process()` cuts the input into blocks of `symbollen` samples, starting at `x[0]`
of every call; the last block of a call may be shorter. `symbollen` is
`(int)(sample_rate / baud + 0.5)` as in fldigi (`rtty.cxx:256`); 176 samples for 45.45 Bd at
8 kHz. All per-block quantities (mixer step including the AFC offset) are fixed for one
block, so an AFC correction takes effect at the next block, that is within the current
character.

### 2. Mark and space mixers

fldigi: `rtty::mixer`, `rtty.cxx:393-401`, called at `rtty.cxx:681-685`. C: the two loops at
the start of `block()`.

Each tone is shifted to 0 Hz by multiplying with `e^{j phi}` and decrementing the phase by
`2 pi (f + offset) / sample_rate` per sample. The phase is set exactly at each block start
and kept in `[0, 2 pi)`; within a block the phasor is advanced by complex multiplication.
fldigi feeds `cmplx(x, x)` into the mixers; this core feeds `x` (real). The difference is a
constant factor `sqrt(2) e^{j pi/4}` that does not change any decision.

Which of the two tones is "mark" is decided only by `mark_hz` and `space_hz`. fldigi
inverts the bits for LSB or "reverse" (`rtty.cxx:648`, `rtty.cxx:845`); this core has no
reverse flag: swap `mark_hz` and `space_hz` instead.

### 3. Raised-cosine RTTY filter

fldigi: `fftfilt::rtty_filter`, `fftfilt.cxx:245-290` (shaping constant `K = 1.4` at
`fftfilt.cxx:263`), filter construction in `rtty::reset_filters`, `rtty.cxx:221-229`,
filter length table `rtty.cxx:95-98`. C: `rtty_filter_taps()` (design, at create time) and
`filter_block()` (run time).

fldigi designs the filter in the frequency domain on a 512-bin grid: a cos² roll-off up to
`2 f` (with `f = 1.4 baud / sample_rate`), equalised by `1 / sinc(2 i f)` ("amplitude
equalised Nyquist channel"), with a linear phase of `-pi/2` per bin. fldigi applies it by
overlap-add with 256-sample blocks (`fftfilt::run`, `fftfilt.cxx:202-237`).

This core computes the equivalent FIR: the real part of the inverse DFT of the same
512-bin response, first 256 samples, i.e. 256 taps with a delay of 128 samples (16 ms at
8 kHz). It applies the taps to each symbol block by overlap-save (zero-padded FFT of
`[history | block]`, `nfft` = next power of two ≥ `255 + symbollen`, 512 at 45.45 Bd /
8 kHz), which equals the direct-form convolution up to rounding. The taps are exposed by
`frtty_filter_taps()` and checked against the golden taps.

### 4. Envelope and noise trackers

fldigi: `rtty.cxx:694-708`, `decayavg` in `src/include/misc.h:59-63`. C: the per-sample
loop in `block()`.

For each tone magnitude `m = |z|`:

- envelope: `env += (m - env) / w`, `w = symbollen/4` if `m > env`, else `symbollen*16`
- noise floor: `noise += (m - noise) / w`, `w = symbollen/4` if `m < noise`, else `symbollen*48`

`floor = min(mark_noise, space_noise)`; the magnitudes are clipped to `env` from above and
to `floor` from below (`rtty.cxx:710-715`).

### 5. Optimal ATC bit decision

fldigi: `rtty.cxx:737-740` and `760` (labelled "Optimal ATC", automatic threshold
correction; fldigi also keeps the unused linear/clipped/Kahn variants as comments). C: `v3`
in `block()`.

```
v3 = (mc - floor)(me - floor) - (sc - floor)(se - floor)
     - 0.25 ((me - floor)^2 - (se - floor)^2)
bit = v3 > 0        (1 = mark)
```

The decision is homogeneous in the signal amplitude (all terms are products of two
magnitudes), so it does not depend on the input scale; see [Input format](#input-format).

### 6. Framing state machine

fldigi: `rtty::rx`, `rtty.cxx:484-553`, with `is_mark_space` (`rtty.cxx:465-477`) and
`is_mark` (`rtty.cxx:479-482`). C: `rx_bit()`; the bit buffer is a circular buffer in
`block()` (fldigi shifts an array, `rtty.cxx:490-491`).

Every sample pushes one bit into a buffer of `symbollen` bits; the number of ones is kept
as a running count.

| State | Action |
|-------|--------|
| IDLE | If the oldest bit is mark and the newest is space, and the number of ones is within 6 of `symbollen/2` (the mark/space edge is near the middle), go to START with `counter = ones`. |
| START | Count down; at 0 sample the middle bit of the buffer. Space: go to DATA (`counter = symbollen`). Mark: false start, back to IDLE. |
| DATA | Every `symbollen` samples sample the middle bit and store it as bit `bitcntr` of the code (first data bit on air = bit 0, LSB first). After `nbits` bits go to STOP. |
| STOP | After one more symbol sample the middle bit. Mark: emit the code. Space: drop it (framing error). Back to IDLE in both cases. |

Not ported: parity bits (fldigi `rtty.cxx:517` counts a parity bit for 7/8-bit codes), the
squelch gate (`rtty.cxx:523`), and the character decoding (`decode_char`) and output.

### 7. SNR estimate

C: at each emitted code `block()` adds `max(mark_env, space_env) - floor` to a signal
sum and `floor` (at least `1e-12`) to a noise sum; `frtty_rx_snr_db()` returns
`10 log10(signal_sum / noise_sum)`. This is the estimator of the former Python reference,
not fldigi's `rtty::Metric()` (which uses waterfall power densities). It is 0 until the
first code.

### 8. AFC (optional)

fldigi: `rtty.cxx:845-866`. C: `afc()`, called from `block()` when a code is emitted and
`cfg.afc` is nonzero.

At the stop bit of each emitted code, the frequency error is measured from the rotation
of the mark filter output over one sample: `ferr = arg(conj(z[k-1]) z[k]) * sample_rate /
(2 pi)`. Measurements with `|ferr| > baud/2` are ignored; otherwise
`offset += ferr / 8` (`decayavg` with weight 8), clamped to `±afc_range_hz`. The offset is
added to both mixer frequencies from the next block on. This follows the structure of
fldigi's AFC (measure at the decoded character, reject large errors, average with weight 8)
in the formulation of the former Python reference; fldigi itself scales the error
differently and retunes its waterfall carrier via `set_freq()`.

### Differences from fldigi

The core reproduces the former Python reference bit for bit on the goldens. Where that
reference differs from fldigi 4.2.13, the core inherits the difference:

- Filter application as an equivalent 256-tap FIR (no 256-sample block latency of
  `fftfilt::run` and no suppression of the first two output blocks).
- The filter grid is always 512 bins (256 taps). fldigi uses 512 for 45-110 Bd and 256,
  128, 64 for 150, 200, 300 Bd (`rtty.cxx:97-98`).
- Tracker weights are `symbollen / 4.0` as doubles; fldigi passes integers (`symbollen / 4`
  in integer arithmetic). Identical when `symbollen` is a multiple of 4 (176 at 45.45 Bd,
  160 at 50 Bd, both at 8 kHz), slightly different otherwise (e.g. 107 at 75 Bd).
- No reverse flag, no parity, no squelch, no mark-only/space-only decoding
  (`progdefaults.rtty_cwi`, `rtty.cxx:717-724`); own SNR estimator; AFC as described above.

## Configuration reference

```c
typedef struct frtty_config {
    double sample_rate;
    double mark_hz;
    double space_hz;
    double baud;
    int nbits;
    int afc;
    double afc_range_hz;
} frtty_config;
```

| Field | Unit | Checked by `frtty_rx_create()` | wave-owl | Notes |
|-------|------|-------------------------------|----------|-------|
| `sample_rate` | Hz | `> 0` | 8000 | fldigi's RTTY modem runs at 8000 Hz (`RTTY_SampleRate`, `src/include/rtty.h:45`). Use 8 kHz: the filter grid is fixed at 512 bins, so its frequency resolution gets coarser at higher rates. |
| `mark_hz` | Hz | not checked | 2295 | Mark tone in the audio. Amateur RTTY sends mark on the higher RF frequency, so on USB audio mark is the upper tone. |
| `space_hz` | Hz | not checked | 2125 | Space tone. Shift = `|mark_hz - space_hz|`. |
| `baud` | symbols/s | `> 0`, and `symbollen = (int)(sample_rate / baud + 0.5) >= 2` | 45.45 | Determines `symbollen` and the filter shape. |
| `nbits` | bits | `1..8` | 5 | Data bits per character, no parity. |
| `afc` | flag | not checked | 0 (off) | Nonzero enables AFC. |
| `afc_range_hz` | Hz | `>= 0` | 50 | Maximum magnitude of the AFC offset. Ignored when `afc` is 0. |

`frtty_rx_create()` returns `NULL` for a `NULL` config, any failed check above (NaN fails
the `> 0` / `>= 0` checks), or when an allocation fails. Tone frequencies are not
validated: tones at or above `sample_rate / 2`, equal tones or negative values are
accepted and simply give a receiver that decodes nothing useful.

**Other values.** The code handles any shift (170, 425, 850 Hz, ...) because each tone has
its own mixer, and any baud rate that passes the checks (50, 75 Bd, ...); the filter
bandwidth scales with `baud / sample_rate`, its length (256 taps) does not. Only 45.45 Bd,
170 Hz shift, 5 bits at 8 kHz is verified against golden data; other settings run the same
code but are not compared against fldigi (see also
[Differences from fldigi](#differences-from-fldigi)).

## API reference

All declarations are in `include/fldigi_rtty.h`.

| Function | Description |
|----------|-------------|
| `const char *frtty_version(void)` | Version and contract string, e.g. `"fldigi-rtty-core 0.1.1 contract=rtty-c1 abi=1"`. Static string, never freed. |
| `frtty_rx *frtty_rx_create(const frtty_config *cfg)` | New receiver in its initial state. The config is copied; `cfg` may be freed afterwards. `NULL` on invalid config or out of memory. |
| `void frtty_rx_destroy(frtty_rx *rx)` | Free the receiver. `NULL` is allowed. |
| `void frtty_rx_reset(frtty_rx *rx)` | Back to the state right after create: mixer phases, filter history, trackers, framing, SNR sums and AFC offset are cleared. Tones keep their last values (from `set_tones`). `NULL` is ignored. |
| `size_t frtty_rx_process(frtty_rx *rx, const double *x, size_t n, uint8_t *codes, size_t max_codes)` | Process `n` samples. Writes at most `max_codes` codes to `codes` and returns the number of codes received; codes beyond `max_codes` are counted in the return value but dropped. `codes` may be `NULL` only if `max_codes` is 0. Returns 0 if `rx` or `x` is `NULL`. |
| `void frtty_rx_set_tones(frtty_rx *rx, double mark_hz, double space_hz)` | Retune both tones; effective from the next block. All other state (filters, trackers, framing, AFC offset) is kept. Not validated. |
| `double frtty_rx_snr_db(const frtty_rx *rx)` | SNR estimate in dB since create/reset (see [SNR estimate](#7-snr-estimate)); 0 before the first code. |
| `double frtty_rx_offset_hz(const frtty_rx *rx)` | Current AFC offset in Hz; 0 without AFC. |
| `size_t frtty_filter_len(const frtty_rx *rx)` | Number of filter taps (256). |
| `const double *frtty_filter_taps(const frtty_rx *rx)` | The filter taps, owned by the receiver, valid until `frtty_rx_destroy()`. |

### Output codes

Each code is `0 .. 2^nbits - 1`, first data bit on air in bit 0. For Baudot this is the
ITA2 code index as used by fldigi's tables (`rtty.cxx:62-87`). A suitable buffer size for
one call is `n / symbollen + 8`: one character needs at least `nbits + 2` symbols.

### Ownership and threads

The caller owns the receiver handle and all buffers it passes in; the library keeps no
pointer to `x` or `codes` after a call returns. A receiver is not internally synchronised:
use one receiver from one thread at a time. Independent receivers share nothing (there is
no global or static mutable state; the only file-scope objects are `static const`
constants), so different threads may run different receivers concurrently.

### Chunking

The input may be split into calls of any length, including 1 sample. Without AFC the
codes do not depend on the split (tested for chunks of 1, 7, 333 and 1000
samples; the SNR can differ in the last digits from FFT rounding). With AFC they can,
because blocks restart at `x[0]` of every call and the AFC offset is applied per block (the golden cases `rtty_cq_afc20` and
`rtty_cq_afc20_chunk1000` give the same codes but slightly different SNRs). Feed AFC
receivers in a fixed chunking if bit-exact repeatability across runs matters.

### Input format

Mono, real, `double`, at `sample_rate`. The scale does not matter: the trackers, the ATC
decision, the SNR and the AFC are all invariant to a constant gain (scaling the golden
input by 0.001, 0.3, 7 or 1000 gave identical codes and SNR). The tests use 16-bit PCM
divided by 32768 and float64 audio.

## Usage examples

### C

Reads raw little-endian 16-bit mono audio at 8 kHz from stdin and prints one code per
line.

```c
/* rtty_codes.c: raw s16le mono audio on stdin -> one 5-bit code per line on stdout. */
#include <stdint.h>
#include <stdio.h>

#include "fldigi_rtty.h"

int main(void)
{
    frtty_config cfg = {
        8000.0,   /* sample_rate */
        2295.0,   /* mark_hz (upper tone on USB) */
        2125.0,   /* space_hz */
        45.45,    /* baud */
        5,        /* nbits */
        0,        /* afc */
        50.0,     /* afc_range_hz */
    };
    frtty_rx *rx = frtty_rx_create(&cfg);
    unsigned char raw[2 * 4096];
    double x[4096];
    uint8_t codes[256];
    size_t nbytes, n, i, got;

    if (!rx) {
        fprintf(stderr, "invalid configuration\n");
        return 1;
    }
    while ((nbytes = fread(raw, 1, sizeof raw, stdin)) >= 2) {
        n = nbytes / 2;  /* a trailing odd byte is dropped */
        for (i = 0; i < n; i++)
            x[i] = (int16_t)(uint16_t)(raw[2 * i] | (raw[2 * i + 1] << 8)) / 32768.0;
        got = frtty_rx_process(rx, x, n, codes, sizeof codes);
        for (i = 0; i < got && i < sizeof codes; i++)
            printf("%u\n", codes[i]);
    }
    fprintf(stderr, "SNR %.1f dB\n", frtty_rx_snr_db(rx));
    frtty_rx_destroy(rx);
    return 0;
}
```

```sh
make
cc -std=c99 -Iinclude rtty_codes.c build/libfldigi_rtty.a -lm -o rtty_codes
sox recording.wav -t raw -r 8000 -c 1 -e signed -b 16 -L - | ./rtty_codes
```

The golden recording `tests/golden/rtty_cq_clean.s16` has its tones at 1585/1415 Hz; with
those values in `cfg` the program prints the 34 codes listed for it in
`tests/golden/expected.txt`.

### Python (ctypes)

```python
import ctypes
import sys

import numpy as np


class Config(ctypes.Structure):
    _fields_ = [("sample_rate", ctypes.c_double), ("mark_hz", ctypes.c_double),
                ("space_hz", ctypes.c_double), ("baud", ctypes.c_double),
                ("nbits", ctypes.c_int), ("afc", ctypes.c_int),
                ("afc_range_hz", ctypes.c_double)]


lib = ctypes.CDLL("build/libfldigi_rtty.dylib")  # .so on Linux
lib.frtty_version.restype = ctypes.c_char_p
lib.frtty_rx_create.restype = ctypes.c_void_p
lib.frtty_rx_create.argtypes = [ctypes.POINTER(Config)]
lib.frtty_rx_destroy.argtypes = [ctypes.c_void_p]
lib.frtty_rx_process.restype = ctypes.c_size_t
lib.frtty_rx_process.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_double),
                                 ctypes.c_size_t, ctypes.POINTER(ctypes.c_uint8),
                                 ctypes.c_size_t]

version = lib.frtty_version().decode()
if "contract=rtty-c1" not in version or "abi=1" not in version.split():
    sys.exit("incompatible library: " + version)

rx = lib.frtty_rx_create(ctypes.byref(Config(8000, 1585, 1415, 45.45, 5, 0, 50)))
x = np.fromfile("tests/golden/rtty_cq_clean.s16", dtype="<i2") / 32768.0
out = (ctypes.c_uint8 * 4096)()
n = lib.frtty_rx_process(rx, x.ctypes.data_as(ctypes.POINTER(ctypes.c_double)), x.size,
                         out, len(out))
print(list(out[:min(n, len(out))]))
lib.frtty_rx_destroy(rx)
```

### Production binding: wave-owl

wave-owl's binding is
[`src/rtty_native.py`](https://github.com/wave-owl/wave-owl/blob/main/src/rtty_native.py):

- **Lookup order:** `$WAVE_OWL_RTTY_LIB` (must exist if set) -> the app bundle
  (PyInstaller) -> the development build directories (`third_party/build-universal`, then
  `third_party/build`, filled by `make build-rtty-core`).
- **Compatibility check:** after loading, `frtty_version()` must contain
  `contract=rtty-c1` and `abi=1`; otherwise the library is rejected.
- **No fallback:** the library is loaded once per process. If it is missing or
  incompatible, the reason is cached and reported in `/api/status`, and RTTY is
  unavailable (creating an RTTY consumer raises `RuntimeError`); all other modes keep
  running. There is no Python receiver to fall back to.
- **Use:** `NativeFskDemodulator` wraps one receiver (8000 Hz, mark 2295 Hz, space 2125 Hz,
  45.45 Bd, 5 bits, AFC off by default). `RttyReceiver` adds `src/modes/rtty/ita2.py`
  (US-TTY figures, unshift-on-space) for text. `src/rtty_consumer.py` down-converts the
  receiver IQ to 8 kHz USB audio around the RTTY dial frequency and feeds it to the
  demodulator.

## Contract and versioning

- **Contract** `rtty-c1` (`FRTTY_CONTRACT`): the behaviour documented here, i.e. the codes
  and SNR the receiver produces for a given input and configuration, as fixed by the
  goldens, and the semantics of each function.
- **ABI** `1` (`FRTTY_ABI`): the binary interface: function signatures and the layout of
  `frtty_config`.
- **Version string** (`frtty_version()`):
  `fldigi-rtty-core <major>.<minor>.<patch> contract=rtty-c1 abi=1`. Hosts must check the
  `contract=` and `abi=` tokens, not the version number.

Semantic versioning, with the following rules:

| Change | Version | Contract | ABI |
|--------|---------|----------|-----|
| Documentation, build, tests, refactoring with bit-identical goldens | patch | same | same |
| New function, existing ones unchanged | minor | same | same |
| Deliberate change of output (codes or SNR on the goldens, e.g. after porting an fldigi change), or of a function's semantics | minor (major after 1.0) | new (`rtty-c2`) | same unless the interface changes |
| Changed signature, removed function or changed `frtty_config` layout | major (minor before 1.0) | new | new |

When the ABI changes, both `FRTTY_ABI` in the header and the literal `abi=` in
`frtty_version()` (`src/rtty_rx.c`) must be updated; the version number is `FRTTY_VERSION`
in `src/rtty_rx.c`.

## Verification

### Golden data

`tests/golden/` holds input audio and expected output, frozen from wave-owl's former Python
receiver `src/modes/rtty/demod.py` at wave-owl commit
`b6be59bfb748e4f3457b421aa362d26c34ca49d0`. That Python file has since been removed from
wave-owl; this C core is now the reference, and the goldens change only deliberately
(together with a contract bump, see [Contract and versioning](#contract-and-versioning)).

`tests/golden/make_golden.py /path/to/wave-owl` regenerates the files from wave-owl's
`src/modes/conformance/rtty_golden.py` (input signals) and
`tests/fixtures/rtty_golden/golden.json` (frozen codes, SNRs and taps). Formats:

- `<name>.s16`: little-endian int16 raw audio (sample = value / 32768), used when the
  float audio round-trips exactly; otherwise `<name>.f64`, little-endian float64.
- `expected.txt`: `name mark_hz space_hz afc chunk(0=one call) snr_db file codes`
  (`-` = no codes).
- `taps.txt`: the 256 filter taps, one `%.17g` per line.

| Case | Content |
|------|---------|
| `rtty_cq_clean` | wave-owl's fldigi 4.2.13 reference vector (`src/modes/rtty/vectors/rtty_cq.wav`, `CQ CQ DE DG6FL DG6FL K` transmitted by fldigi 4.2.13), 18.9 s of 45.45 Bd / 170 Hz RTTY, mark 1585 Hz, space 1415 Hz |
| `rtty_cq_m5`, `rtty_cq_m10` | the same with noise added to -5 and -10 dB SNR (wave-owl bench definition); the expected codes include the errors the receiver makes |
| `e_frame` | synthetic idle mark, one `E` frame (code 1), idle mark, at 2295/2125 Hz |
| `e_frame_mirror` | the same frame with the tones swapped: no code |
| `rtty_cq_afc20` | the clean vector moved up by 20 Hz, receiver with AFC, one call |
| `rtty_cq_afc20_chunk1000` | the same in calls of 1000 samples (pins the per-call blocking of the AFC) |

### Tests

`make test` builds `build/test_rtty` and runs it on `tests/golden`. It checks:

- the version string contains `contract=rtty-c1 abi=1`;
- the 256 filter taps against `taps.txt` (maximum error ≤ 1e-12);
- every golden case: codes exactly, SNR within 0.01 dB;
- chunk invariance without AFC (chunks of 1, 7, 333, 1000 samples vs one call);
- `max_codes` truncation (return value counts all codes, the first 4 are written) and
  `max_codes = 0` with `codes = NULL`;
- invalid configurations (`baud = 0`, `nbits = 0`, `NULL` config) return `NULL`;
- `frtty_rx_reset()` reproduces a fresh run, with and without AFC;
- `frtty_rx_set_tones()` on a receiver created with other tones gives the golden codes.

### Sanitizers

The test suite runs clean under AddressSanitizer and UndefinedBehaviorSanitizer:

```sh
make BUILD=build-san \
     CFLAGS="-O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=undefined" \
     LDFLAGS="-fsanitize=address,undefined" test
```

This is a manual check; CI does not run it.

### CI

[`.github/workflows/ci.yml`](.github/workflows/ci.yml) runs `make` and `make test` on every
push and pull request, on `ubuntu-latest` and `macos-latest` (default compilers, `-O3`).

### Floating point

The library is compiled with `-ffp-contract=off`: the compiler must not fuse `a * b + c`
into one fused multiply-add, which rounds differently. The operations then follow the
order of the reference, so results agree across compilers and CPUs (x86-64 and arm64 in CI)
to the tolerances above. The framing makes the output sensitive to tiny differences near
a decision threshold, which is why this matters for an exact code comparison.

## Performance and memory

Measured on an Apple M3 Pro with wave-owl's
`openspec/changes/add-fldigi-rtty-core/bench/bench_rtty_cpu.py`, golden case
`rtty_cq_clean` (18.9 s of 8 kHz audio), best of 5 runs:

| Receiver | CPU time | Share of real time |
|----------|----------|--------------------|
| Python reference (wave-owl, numpy/scipy) | 149.5 ms | 0.79 % |
| fldigi-rtty-core | 13.8 ms | 0.073 % |

The core needs about 9 % of the CPU time of the Python reference.

Memory per receiver at 8 kHz / 45.45 Bd (`symbollen` 176, 256 taps, FFT length 512):
the 272-byte receiver struct plus 10 heap buffers, about **39.6 KB** in total (40.4 KB
including allocator rounding on macOS). The largest parts are the filter spectrum and
FFT work buffer (8 KB each), the filter histories of both branches (8 KB together), the
twiddle table (4 KB) and the per-block filter outputs (5.6 KB). The size shrinks slightly
with higher baud rates (about 35 KB at 100 Bd). `frtty_rx_create()` additionally uses
8 KB of temporary memory for the filter design and evaluates a 512 x 256-point DFT; create
receivers once, not per call.

## Building

Requirements: a C99 compiler, `make`, `ar`, and `libm`. The Makefile builds shared
libraries for macOS (`.dylib`, install name `@rpath/libfldigi_rtty.dylib`) and for other
Unix systems (`.so`, soname `libfldigi_rtty.so`); there is no Windows target.

| Command | Result |
|---------|--------|
| `make` | `build/libfldigi_rtty.a` and the shared library |
| `make static` / `make shared` | only one of them |
| `make test` | build and run `build/test_rtty tests/golden` |
| `make clean` | remove the build directory |
| `make BUILD=/abs/dir shared` | out-of-tree build (wave-owl builds into `third_party/build/fldigi-rtty-core`, so the submodule checkout stays clean) |
| `make CFLAGS="-O2 -g"` | override optimisation/debug flags; `-std=c99 -Wall -Wextra -Werror -pedantic -ffp-contract=off -fPIC` are always added |

Objects are compiled with `-fPIC`, so the static library can be linked into shared
objects.

**macOS universal binary.** The Makefile builds for the host architecture. wave-owl's
`make build-universal` builds the shared library twice and joins them:

```sh
make BUILD=$PWD/build-arm64  CFLAGS="-O3 -arch arm64"  LDFLAGS="-arch arm64"  shared
make BUILD=$PWD/build-x86_64 CFLAGS="-O3 -arch x86_64" LDFLAGS="-arch x86_64" shared
lipo -create build-arm64/libfldigi_rtty.dylib build-x86_64/libfldigi_rtty.dylib \
     -output libfldigi_rtty.dylib
```

## Maintenance and fldigi updates

### Provenance

- Upstream: fldigi, <https://git.code.sf.net/p/fldigi/fldigi>, tag `v4.2.13`, commit
  `e7c3ab3709ad62a7d91829364eb48a05fa325c0d`
- Mirror: <https://github.com/wave-owl/fldigi>
- Former Python reference: wave-owl `src/modes/rtty/demod.py` at commit
  `b6be59bfb748e4f3457b421aa362d26c34ca49d0` (removed since)

Every ported block in `src/rtty_rx.c` carries a `file.cxx:a-b` citation of the fldigi
source it was ported from (`rtty.cxx`, `fftfilt.cxx`, `misc.h`).

### Updating to a new fldigi release

1. In a wave-owl checkout run `python scripts/fldigi_upstream_check.py --new <tag>`. It
   collects the citations of all fldigi ports (including this core as
   `third_party/fldigi-rtty-core`), diffs the cited files between the pinned tag and the
   new one in the mirror, and classifies each citation as `unchanged`, `shifted` (content
   identical, line numbers moved), `changed` (a hunk touches the cited lines) or `gone`
   (file removed). It also lists changes in the ported fldigi directories that no
   citation covers, and the files that record the old pin.
2. Renumber `shifted` citations. Review `changed`, `gone` and uncited changes; port
   behavioural changes to `src/rtty_rx.c`.
3. If the output changes, regenerate the goldens deliberately and bump the contract (see
   [Contract and versioning](#contract-and-versioning)). The goldens are no longer
   produced from a Python receiver; new expected values come from this core after review
   (for example by comparing with fldigi itself on the same audio).
4. Update the provenance above and the header comments, bump `FRTTY_VERSION`, add a
   changelog entry, tag the release, and update the submodule in wave-owl.

## Roadmap

- **Transmit:** an RTTY modulator (Baudot codes to audio, after fldigi's `rtty::tx_process`)
  is planned as a wave-owl OpenSpec change.
- **Multi-channel use:** a wave-owl RTTY skimmer that detects signals and runs one receiver
  per signal (OpenSpec change `add-rtty-skimmer`) is deferred. The API already allows many
  independent instances.

## License and credits

GPL-3.0-or-later, see [LICENSE](LICENSE).

Derived from fldigi 4.2.13, Copyright (C) Dave Freese W1HKJ and the fldigi developers
(`src/rtty/rtty.cxx`, `src/filters/fftfilt.cxx`, `src/include/misc.h`). The optimal ATC
is generally attributed to Kok Chen, W7AY; the raised-cosine filter is fldigi's design after K. Feher,
*Telecommunications Measurements, Analysis, and Instrumentation*, section 1.2.6, as
cited in `fftfilt.cxx`. C port: Gerolf Ziegenhain, DG6FL.

## Changelog

See [CHANGELOG.md](CHANGELOG.md).
