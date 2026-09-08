#!/usr/bin/env python3
"""em_assemble.py - assemble the E-M (mailbox v2) crash-rate/timing CSV from
the D3 Phase 3 battery run dirs + gate runs.

An arm counts ALIVE iff its dump is 44000 bytes and byte-identical to the
engref reference (hard data gate; the gate LINE goes to wiki_exp's stdout,
not client.log, so we compare bytes directly).
Output: em_results.csv next to this file.
"""
import csv, os, re

HERE = os.path.dirname(os.path.abspath(__file__))
EXP = "/var/tmp/cylon/anns/exp"
REF = "/var/tmp/cylon/anns/index/wiki/engref_ef100.dump"
OUT = os.path.join(HERE, "em_results.csv")

MODES = ("poll", "sleep", "doorbell")
CELLS = [
    ("scalar", 1000, 0.65),
    ("avx", 250, 0.25),
]
GATES = [
    ("g5_doorbell", "gate", "doorbell", 5, 0.5, 0),
    ("g4_b250_f050_scalar", "gate", "poll", 5, 0.5, 0),
    ("ctrl_v1_f065b1000", "v1", "poll", 5, 0.65, 1000),
]

def parse(tag):
    lp = os.path.join(EXP, tag, "client.log")
    dp = os.path.join(EXP, tag, tag + ".dump")
    log = ""
    if os.path.isfile(lp):
        log = open(lp, encoding="utf-8", errors="replace").read()
    wall_m = re.search(r"wall ([0-9.]+) s", log)
    rec_m = re.search(r"recall@10  = ([0-9.]+)", log)
    if os.path.isfile(dp) and os.path.getsize(dp) == 44000 \
            and open(REF, "rb").read() == open(dp, "rb").read():
        gate = "PASS"
    elif log:
        gate = "DEAD"
    else:
        gate = "NORUN"
    return {
        "alive": gate,
        "wall_s": wall_m.group(1) if wall_m else "",
        "recall": rec_m.group(1) if rec_m else "",
    }

def main():
    rows = []
    for build, bi, f in CELLS:
        for mode in MODES:
            for rep in (1, 2, 3):
                tag = "em_%s_b%d_%s_%d" % (build, bi, mode, rep)
                rec = parse(tag)
                rows.append({
                    "tag": tag, "build": build, "f": f, "bi_ns": bi,
                    "notify": mode, "rep": rep,
                    "alive": rec["alive"], "wall_s": rec["wall_s"],
                    "recall": rec["recall"], "gate": rec["alive"],
                })
    for tag, build, mode, rep, f, bi in GATES:
        rec = parse(tag)
        rows.append({
            "tag": tag, "build": build, "f": f, "bi_ns": bi,
            "notify": mode, "rep": rep,
            "alive": rec["alive"], "wall_s": rec["wall_s"],
            "recall": rec["recall"], "gate": rec["alive"],
        })
    with open(OUT, "w", newline="") as fh:
        w = csv.DictWriter(fh, fieldnames=[
            "tag", "build", "f", "bi_ns", "notify", "rep",
            "alive", "wall_s", "recall", "gate"])
        w.writeheader()
        for r in rows:
            w.writerow(r)
    print("wrote", OUT, len(rows), "rows")

if __name__ == "__main__":
    main()
