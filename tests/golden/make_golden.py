#!/usr/bin/env python3
"""Regenerate the golden test data in this directory from a wave-owl checkout.

Usage (from anywhere, with the wave-owl virtualenv active):

    python tests/golden/make_golden.py /path/to/wave-owl [out_dir]

Inputs come from ``src.modes.conformance.rtty_golden.golden_cases()`` and the frozen
reference codes, SNRs and filter taps from ``tests/fixtures/rtty_golden/golden.json`` of
wave-owl. Outputs:

* ``<name>.s16`` -- little-endian int16 raw audio (value = sample * 32768), only for cases
  whose float audio is exactly int16 / 32768 (verified round trip).
* ``<name>.f64`` -- little-endian float64 raw audio for all other cases; cases with identical
  audio share one file.
* ``expected.txt`` -- one line per case:
  ``name mark_hz space_hz afc chunk(0=none) snr_db file code,code,...`` (``-`` = no codes).
* ``taps.txt`` -- the 256 receive filter taps, one ``%.17g`` per line.
"""

import hashlib
import json
import os
import sys

import numpy as np


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    wave_owl = os.path.abspath(sys.argv[1])
    out = os.path.abspath(sys.argv[2] if len(sys.argv) > 2 else os.path.dirname(__file__))
    sys.path.insert(0, wave_owl)
    os.chdir(wave_owl)
    from src.modes.conformance.rtty_golden import golden_cases

    with open("tests/fixtures/rtty_golden/golden.json") as f:
        ref = json.load(f)
    cases = golden_cases()
    files_by_hash = {}
    lines = []
    for name, ref_case in ref["cases"].items():
        audio = np.ascontiguousarray(cases[name].audio, dtype=np.float64)
        assert len(audio) == ref_case["samples"], name
        digest = hashlib.sha256(audio.tobytes()).hexdigest()
        if "audio_sha256" in ref_case and digest != ref_case["audio_sha256"]:
            sys.exit("audio hash mismatch for %s" % name)
        fname = files_by_hash.get(digest)
        if fname is None:
            scaled = audio * 32768.0
            pcm = np.round(scaled)
            if (np.all(pcm == scaled) and pcm.min() >= -32768 and pcm.max() <= 32767
                    and np.array_equal(pcm.astype("<i2").astype(np.float64) / 32768.0, audio)):
                fname = name + ".s16"
                data = pcm.astype("<i2").tobytes()
            else:
                fname = name + ".f64"
                data = audio.astype("<f8").tobytes()
            with open(os.path.join(out, fname), "wb") as f:
                f.write(data)
            files_by_hash[digest] = fname
        codes = ",".join(str(c) for c in ref_case["codes"]) or "-"
        lines.append("%s %.17g %.17g %d %d %.17g %s %s" % (
            name, ref_case["mark_hz"], ref_case["space_hz"], int(ref_case["afc"]),
            ref_case["chunk"] or 0, ref_case["snr_db"], fname, codes))
    with open(os.path.join(out, "expected.txt"), "w") as f:
        f.write("\n".join(lines) + "\n")
    assert len(ref["taps"]) == 256
    with open(os.path.join(out, "taps.txt"), "w") as f:
        f.write("".join("%.17g\n" % t for t in ref["taps"]))
    print("wrote %d cases to %s (source commit %s)"
          % (len(lines), out, ref.get("source_commit")))


if __name__ == "__main__":
    main()
