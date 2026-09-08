#!/usr/bin/env python3
"""test_f2h.py -- numpy cross-check for libcylon's f32->f16 (RNE).

Semantic reference: numpy astype(float16) is how queries_fp16.bin was
produced, so the library converter must reproduce its bit patterns exactly
(subnormals included, like pnm's qconv path). Pairs come from f2h_main.c:
6,000,030 records of (u32 f32_bits, u16 f16_bits).
"""
import struct
import subprocess
import sys
import os

SDK = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")


def main():
    import numpy as np

    # build the harness (links the real cylon_api.c converter)
    exe = "/tmp/f2h_main"
    r = subprocess.run(
        ["gcc", "-O2", "-fno-tree-vectorize",
         "-I", os.path.join(SDK, "src"),
         "-I", os.path.join(SDK, "include"),
         "-I", os.path.join(SDK, "..", "hw/femu/cylon"),
         os.path.join(SDK, "tests", "f2h_main.c"),
         os.path.join(SDK, "src", "cylon.c"),
         "-o", exe],
        capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stderr)
        sys.exit("build failed")

    out = subprocess.run([exe], capture_output=True).stdout
    n = len(out) // 6
    arr = np.frombuffer(out[: n * 6], dtype=np.uint8).reshape(n, 6)
    f32 = arr[:, :4].copy().view(np.uint32).reshape(-1)
    f16 = arr[:, 4:6].copy().view(np.uint16).reshape(-1)

    with np.errstate(over="ignore", invalid="ignore"):
        ref = f32.view(np.float32).astype(np.float16).view(np.uint16)
    bad = np.nonzero(f16 != ref)[0]
    if len(bad):
        for i in bad[:8]:
            v = f32[i]
            print("MISMATCH f32=0x%08x lib=0x%04x numpy=0x%04x"
                  % (v, f16[i], ref[i]))
        sys.exit("FAIL: %d / %d mismatch" % (len(bad), n))
    print("f2h OK: %d / %d bit-identical to numpy" % (n, n))


if __name__ == "__main__":
    main()
