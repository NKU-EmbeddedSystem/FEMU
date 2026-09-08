#!/usr/bin/env python3
"""plot_es.py — paper figure for Type-2 D2 E-S (device-initiated staging vs
first-touch, wiki_dpr_e5 36GB blob, 512MB devdax cache).

Reads es_results.csv (same dir) and renders:
  panel (a): staging wall per method (log-x bars). first-touch arms bill
             per-page NAND (program 208us cold / read warm); device arms
             bill max(real fill, bytes/bps) with /tmp/femu-stage-bps knob.
  panel (b): search-side invariance — search wall across the same points
             (all within +-3% band), recall fixed 0.9720, all dumps
             byte-identical to engref (gates PASS).

Output: es_staging.png (300 dpi).
"""
import csv
import os

import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

HERE = os.path.dirname(os.path.abspath(__file__))
CSV = os.path.join(HERE, "es_results.csv")
PNG = os.path.join(HERE, "es_staging.png")

rows = [r for r in csv.DictReader(open(CSV)) if r["gate"] == "PASS"]
by = {r["point"]: r for r in rows}

# panel (a): staging wall by method, slowest -> fastest
order = ["es_fresh", "es_ft_restage", "es_slow", "es_t2", "es_t0", "es_t1"]
labels = {
    "es_fresh": "ft cold",
    "es_ft_restage": "ft restage",
    "es_slow": "dev 0.5 GB/s (billed)",
    "es_t2": "dev 14 GB/s",
    "es_t0": "dev floor (unbilled)",
    "es_t1": "dev 2 GB/s (default)",
}
wall = np.array([float(by[p]["stage_wall_s"]) for p in order])
x = np.arange(len(order))

# panel (b): search side across the same points
search = np.array([float(by[p]["search_wall_s"]) for p in order])
recall = np.array([float(by[p]["recall"]) for p in order])
band = 0.03 * search.mean()

fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(7.2, 3.0))

# ---- panel (a): staging wall (log scale) ----------------------------
bars = ax1.bar(x, wall, width=0.62, color=["tab:red", "tab:red",
                                           "tab:blue", "tab:blue",
                                           "tab:blue", "tab:blue"],
               alpha=0.85)
ax1.set_yscale("log")
ax1.set_ylim(10, 3600)
for xi, w in zip(x, wall):
    ax1.text(xi, w * 1.15, f"{w:.1f}s" if w < 1000 else f"{w:.0f}s",
             ha="center", va="bottom", fontsize=7.5)
# speedup arrows: cold -> default, restage -> default
i_cold, i_def = x[0], x[-1]
ax1.annotate("", xy=(i_def, wall[i_def] * 1.7), xytext=(i_cold, wall[i_cold]),
             arrowprops=dict(arrowstyle="->", lw=0.9, color="k",
                             connectionstyle="arc3,rad=-0.25"))
ax1.text(1.6, 1450, f"{wall[i_cold]/wall[i_def]:.0f}x", fontsize=8.5)
ax1.annotate("", xy=(i_def - 0.12, wall[i_def] * 1.35),
             xytext=(x[1], wall[x[1]]),
             arrowprops=dict(arrowstyle="->", lw=0.9, color="0.35",
                             connectionstyle="arc3,rad=-0.2"))
ax1.text(2.15, 560, f"{wall[x[1]]/wall[i_def]:.1f}x", fontsize=8,
         color="0.25")
ax1.set_xticks(x)
ax1.set_xticklabels([labels[p] for p in order], fontsize=7.5,
                    rotation=25, ha="right", rotation_mode="anchor")
ax1.set_ylabel("staging wall time (s, log)")
ax1.set_title("(a) 36GB blob staging: method comparison", fontsize=9)

# ---- panel (b): search-side invariance ------------------------------
ax2.axhspan(search.mean() - band, search.mean() + band,
            color="tab:blue", alpha=0.12, lw=0)
ax2.plot(x, search, "o-", color="tab:blue", lw=1.2, ms=4.5,
         label="search wall")
ax2.set_ylabel("search wall time (s)")
ax2.set_ylim(100, 125)
ax2b = ax2.twinx()
ax2b.plot(x, recall, "s--", color="tab:green", lw=1.0, ms=4,
          label="recall@10")
ax2b.set_ylabel("recall@10")
ax2b.set_ylim(0.96, 0.98)
ax2b.axhline(0.9720, ls=":", lw=0.8, color="tab:green", alpha=0.6)
ax2.set_xticks(x)
ax2.set_xticklabels(["ft\ncold", "dev\n0", "dev\n2e9", "dev\n14e9",
                     "dev\n0.5e9", "ft\nrestage"], fontsize=7)
for xi, s in zip(x, search):
    ax2.text(xi, s + 0.7, f"{s:.1f}", ha="center", fontsize=6.8)
ax2.set_title("(b) search side in-band, gates PASS", fontsize=9)

for ax in (ax1, ax2):
    ax.grid(alpha=0.3, lw=0.4, axis="y")
    for s in ("top", "right"):
        ax.spines[s].set_visible(False)
    ax.tick_params(labelsize=8)
ax2b.spines["top"].set_visible(False)
ax2b.tick_params(labelsize=8)
ax2.legend(handles=[
    plt.Line2D([], [], color="tab:blue", marker="o", lw=1.2,
               label="search wall"),
    plt.Line2D([], [], color="tab:green", marker="s", ls="--", lw=1.0,
               label="recall@10"),
    plt.Rectangle((0, 0), 1, 1, fc="tab:blue", alpha=0.12, lw=0,
                  label="±3% band"),
], fontsize=6.8, frameon=False, loc="lower left")

fig.tight_layout()
fig.savefig(PNG, dpi=300)
print("saved", PNG)
print(f"speedup cold={wall[0]/wall[-1]:.1f}x restage={wall[1]/wall[-1]:.1f}x "
      f"search band={search.min():.1f}-{search.max():.1f}s "
      f"(+-{(search.max()-search.min())/2/search.mean()*100:.1f}%) "
      f"recall={set(recall)}")
