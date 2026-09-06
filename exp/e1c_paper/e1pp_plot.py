#!/usr/bin/env python3
"""Plot the E1'' (BI x f) matrix: wall vs f, one curve per BI knob."""
import csv, matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

SRC = "/var/tmp/cylon/anns/exp/e1pp_matrix.csv"
OUT = "/var/tmp/cylon/anns/exp/e1pp_matrix.png"

rows = [r for r in csv.DictReader(open(SRC)) if r["wall_s"]]
by_bi = {}
for r in rows:
    by_bi.setdefault(int(r["bi_ns"]), []).append((float(r["f"]), float(r["wall_s"])))

fig, ax = plt.subplots(figsize=(7, 4.2), dpi=150)
styles = {0: ("#999", "o", "--"), 250: ("#1f77b4", "s", "-"),
          500: ("#2ca02c", "^", "-"), 1000: ("#d62728", "D", "-")}
for bi, pts in sorted(by_bi.items()):
    pts.sort()
    c, mk, ls = styles[bi]
    ax.plot([p[0] for p in pts], [p[1] for p in pts], color=c, marker=mk, ls=ls,
            lw=1.6, ms=5, label=f"BI={bi}ns")
ax.set_xlabel("CPU fraction f")
ax.set_ylabel("wall (s) for 1000 queries")
ax.set_title("E1'': BI coherence tax across the f-sweep (wiki 21M, 48GB device)")
ax.legend(frameon=False)
ax.grid(alpha=0.3)
fig.tight_layout()
fig.savefig(OUT)
print("wrote", OUT)
