#!/usr/bin/env python3
"""py_demo.py -- Python-path M2 byte-gate client: numpy fp32 queries in via
the ctypes binding (F32 entry), results written in the experiment dump
format (u32 m + k x u32 ids per row) for byte-compare vs engref.
"""
import argparse
import os
import struct
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from cylon import Cylon


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-f", required=True)
    ap.add_argument("-q", required=True)
    ap.add_argument("-o", required=True)
    ap.add_argument("-k", type=int, default=10)
    ap.add_argument("-n", type=int, default=1000)
    ap.add_argument("-e", type=int, default=100)
    args = ap.parse_args()

    q16 = np.fromfile(args.q, dtype=np.uint16)
    q = q16.view(np.float16).astype(np.float32).reshape(-1, 768)
    print("py_demo: queries %s -> fp32 %s" % (q16.shape, q.shape))

    cfg = dict(blob_path=args.f, cpu_frac=0.5, ef=args.e)
    with Cylon(**cfg) as c:
        info = c.info()
        print("device: dim %d ntotal %d storage_prec %d metric %s"
              % (info["dim"], info["ntotal"], info["storage_prec"],
                 info["metric"]))
        q = q16.view(np.float16).astype(np.float32).reshape(-1, info["dim"])
        n = min(args.n, q.shape[0])
        D, L = c.search(q[:n], k=args.k, ef=args.e)
        print("stats:", c.last_stats)
        with open(args.o, "wb") as fp:
            for qi in range(n):
                ids = L[qi]
                m = int((ids != 0xffffffff).sum())
                fp.write(struct.pack("<I", m))
                fp.write(ids.astype(np.uint32).tobytes())
    print("py_demo: dump %s (%d B) written" % (args.o, n * (4 + 4 * args.k)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
