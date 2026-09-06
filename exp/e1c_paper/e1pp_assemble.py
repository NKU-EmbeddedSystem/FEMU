#!/usr/bin/env python3
"""Assemble the E1'' (BI x f) matrix CSV from e1pp_* run dirs.
Winner dirs per cell chosen by gate status; every winner must carry the
BYTE-IDENTICAL gate line in client.log or the assembler aborts."""
import csv, glob, os, re, sys

W = "/var/tmp/cylon/anns"
REF = os.path.join(W, "index/wiki/engref_ef100.dump")
OUT = os.path.join(W, "exp/e1pp_matrix.csv")

# cell -> winning run dir tag (None = dead cell, omitted)
CELLS = {
    ("250", "0"):    "e1pp_b250_f000",
    ("250", "0.25"): "e1pp_b250_f025s",   # avx 4/4 dead -> scalar fallback
    ("250", "0.5"):  "e1pp_b250_f050",
    ("250", "0.65"): "e1pp_b250_f065",
    ("250", "0.75"): "e1pp_b250_f075",
    ("250", "1"):    "e1pp_b250_f100",
    ("500", "0"):    "e1pp_b500_f000r",
    ("500", "0.25"): "e1pp_b500_f025v2",  # avx 2/3 dead -> v2 passed post-reboot
    ("500", "0.5"):  "e1pp_b500_f050",
    ("500", "0.65"): "e1pp_b500_f065",
    ("500", "0.75"): "e1pp_b500_f075",
    ("500", "1"):    "e1pp_b500_f100",
    ("1000", "0"):   "e1pp_b1000_000",    # valid f=0 from the wrong-grid block
    ("1000", "0.25"):"e1pp_b1000_f025",
    ("1000", "0.5"): "e1pp_b1000_f050r",
    ("1000", "0.65"):None,                # 3/3 dead (scalar GPF ip 0x25d0, 3 ASLR bases)
    ("1000", "0.75"):"e1pp_b1000_f075",
    ("1000", "1"):   "e1pp_b1000_f100",
}
FSTR = {"000": "0", "025": "0.25", "050": "0.5", "065": "0.65", "075": "0.75", "100": "1"}

def parse(tag):
    log = os.path.join(W, "exp", tag, "client.log")
    txt = open(log).read()
    dump = os.path.join(W, "exp", tag, f"{tag}.dump")
    if not (os.path.exists(dump) and os.path.getsize(dump) == 44000
            and open(dump, "rb").read() == open(REF, "rb").read()):
        sys.exit(f"ABORT: {tag} dump missing or != 44000B or differs from wiki-ref")
    row = {"point": tag, "dump_vs_engref": "identical"}
    m = re.search(r"collab    : wall ([\d.]+) s  combined ([\d.]+) QPS  "
                  r"\(T_cpu ([\d.]+) s  T_eng ([\d.]+) s\)", txt)
    if m:
        row.update(wall_s=m[1], qps=m[2], t_cpu_s=m[3], t_eng_s=m[4])
    else:
        q = re.search(r"QPS        = ([\d.]+)", txt)
        if not q:
            sys.exit(f"ABORT: {tag} has neither collab line nor QPS")
        wall = round(1000 / float(q[1]), 3)
        row.update(wall_s=wall, qps=q[1], t_cpu_s="", t_eng_s="")  # f=0 / f=1 anchor rows
    for pat, key in [
        (r"engine/query: dist ([\d.]+)\s+hops ([\d.]+)\s+exec (\d+) ns  misses ([\d.]+)", "eng"),
        (r"recall@10  = ([\d.]+)", "recall"),
    ]:
        m = re.search(pat, txt)
        if m:
            if key == "eng":
                row["eng_dist_per_q"], row["eng_hops_per_q"] = m[1], m[2]
                row["exec_ms_per_q"], row["eng_misses_per_q"] = round(int(m[3]) / 1e6, 1), m[4]
            else:
                row["recall"] = m[1]
    return row

rows = []
for (knob, f), tag in CELLS.items():
    if tag is None:
        rows.append({"point": f"(dead: f={f} bi={knob} scalar GPF 3/3, ip 0x25d0)",
                     "bi_ns": knob, "f": f})
        continue
    r = parse(tag)
    r["bi_ns"], r["f"] = knob, f
    SCALAR_TAGS = {"e1pp_b1000_000", "e1pp_b1000_f025", "e1pp_b1000_f050r",
                   "e1pp_b1000_f075", "e1pp_b1000_f100", "e1pp_b250_f025s"}
    r["client"] = "scalar" if tag in SCALAR_TAGS else "avx"
    rows.append(r)

cols = ["point", "bi_ns", "f", "client", "wall_s", "qps", "t_cpu_s", "t_eng_s",
        "eng_dist_per_q", "eng_hops_per_q", "exec_ms_per_q", "eng_misses_per_q",
        "recall", "dump_vs_engref"]
rows.sort(key=lambda r: (int(r["bi_ns"]), float(r["f"])))
with open(OUT, "w", newline="") as fh:
    wtr = csv.DictWriter(fh, fieldnames=cols, extrasaction="ignore")
    wtr.writeheader()
    wtr.writerows(rows)
print(f"wrote {OUT} ({len(rows)} rows)")
for r in rows:
    print(f'  bi={r["bi_ns"]:>4} f={r["f"]:>4} client={r.get("client","-"):6} '
          f'wall={r.get("wall_s","-"):>8} qps={r.get("qps","-"):>4} '
          f't_cpu={r.get("t_cpu_s","-"):>8} t_eng={r.get("t_eng_s","-"):>8}')
