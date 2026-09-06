#!/usr/bin/env python3
"""plot_e1c.py — paper figure for Phase C E1' (wiki_dpr_e5 21M x 768d, 48GB
device, default device profile: comp_dly=0 / pg_rd_lat=40us / PCIe4-x8 SSD).

Reads e1c_results.csv (same dir) and renders:
  panel (a): collab throughput vs CPU-side fraction f, ideal overlap model
             wall(f) = max(f*Tcpu, (1-f)*Teng) as the dashed reference;
  panel (b): wall time and its two legs (1000*f*Tcpu / 1000*(1-f)*Teng),
             showing the X-crossing at f*.

Output: e1c_qps_vs_f.png (300 dpi).
"""
import csv
import os

import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

HERE = os.path.dirname(os.path.abspath(__file__))
CSV = os.path.join(HERE, "e1c_results.csv")
PNG = os.path.join(HERE, "e1c_qps_vs_f.png")

rows = list(csv.DictReader(open(CSV)))
sw = [r for r in rows if r["mode"] == "avx" and r["f"] != ""]
sw.sort(key=lambda r: float(r["f"]))
f = np.array([float(r["f"]) for r in sw])
qps = np.array([float(r["qps"]) for r in sw])
wall = np.array([float(r["wall_s"]) for r in sw])

t_cpu = wall[f == 1.0][0] / 1000.0          # avx pure-CPU per-query (s)
t_eng = wall[f == 0.0][0] / 1000.0          # pure-engine per-query (s)
g = np.linspace(0, 1, 401)
qps_model = 1000.0 / (1000.0 * np.maximum(g * t_cpu, (1 - g) * t_eng))
wall_model = 1000.0 * np.maximum(g * t_cpu, (1 - g) * t_eng)

i_pk = int(np.argmax(qps))
f_pk, q_pk = f[i_pk], qps[i_pk]
speedup = q_pk / qps[f == 1.0][0]
q_ideal_pk = 1000.0 / (1000.0 * max(f_pk * t_cpu, (1 - f_pk) * t_eng))

fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(7.2, 2.9))

# ---- panel (a): throughput vs f ------------------------------------
ax1.plot(g, qps_model, "--", color="0.55", lw=1.2,
         label="ideal overlap model")
ax1.plot(f, qps, "o-", color="tab:blue", lw=1.2, ms=4.5,
         label="measured (AVX2)")
ax1.axhline(1 / t_cpu, ls=":", lw=0.9, color="0.4")
ax1.axhline(1 / t_eng, ls=":", lw=0.9, color="0.4")
ax1.text(0.985, 1 / t_cpu - 0.12, "pure CPU 4.6", ha="right", va="top",
         fontsize=7, color="0.35")
ax1.text(0.985, 1 / t_eng + 0.10, "pure engine 4.8", ha="right", va="bottom",
         fontsize=7, color="0.35")
ax1.annotate(f"f*={f_pk:.2f}\n{q_pk:.1f} QPS ({speedup:.1f}x CPU)",
             xy=(f_pk, q_pk), xytext=(0.60, 9.7),
             arrowprops=dict(arrowstyle="->", lw=0.8), fontsize=7.5)
ax1.set_xlabel("f  (CPU-side query fraction)")
ax1.set_ylabel("throughput (QPS)")
ax1.set_ylim(0, 10.8)
ax1.set_title("(a) CPU+engine collaboration sweep", fontsize=9)

# ---- panel (b): wall time and legs ---------------------------------
ax2.plot(g, g * 1000 * t_cpu, "-", lw=0.9, color="tab:red",
         label=r"CPU leg  $1000f\,T_{cpu}$")
ax2.plot(g, (1 - g) * 1000 * t_eng,
         "-", lw=0.9, color="tab:green",
         label=r"engine leg  $1000(1-f)\,T_{eng}$")
ax2.plot(g, wall_model, "--", color="0.55", lw=1.2, label="wall = max")
ax2.plot(f, wall, "o", color="k", ms=4.5, label="measured wall")
ax2.set_xlabel("f  (CPU-side query fraction)")
ax2.set_ylabel("wall time (s)")
ax2.set_ylim(0, 260)
ax2.set_title("(b) wall time decomposition", fontsize=9)

for ax in (ax1, ax2):
    ax.grid(alpha=0.3, lw=0.4)
    ax.set_xlim(-0.03, 1.03)
    for s in ("top", "right"):
        ax.spines[s].set_visible(False)
    ax.tick_params(labelsize=8)
ax1.legend(fontsize=7, frameon=False, loc="upper left")
ax2.legend(fontsize=7, frameon=False, loc="upper center")

fig.tight_layout()
fig.savefig(PNG, dpi=300)
print("saved", PNG)
print(f"t_cpu={t_cpu*1000:.1f}ms t_eng={t_eng*1000:.1f}ms "
      f"peak f={f_pk} {q_pk} QPS speedup={speedup:.2f}x "
      f"(ideal {q_ideal_pk:.1f} -> {q_pk/q_ideal_pk*100:.0f}% of ideal)")
