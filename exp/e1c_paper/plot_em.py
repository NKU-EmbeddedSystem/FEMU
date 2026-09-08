#!/usr/bin/env python3
"""plot_em.py - paper figure for Type-2 D3 E-M (mailbox v1 poll vs v2
{poll, sleep, doorbell} at the two crash-family poison cells).

Panel (a): scalar poison cell f=0.65 @ BI=1000ns (poll-storm family):
           v2 survival per wait mode (k/3 arms with byte-identical dumps),
           wall times annotated; the CONTROL arm (v1-protocol client vs the
           same v2 engine at the same cell) is annotated on the right.
Panel (b): avx poison cell f=0.25 @ BI=250ns (flip/flush family): same
           arms; family is wait-mode-independent (translation-layer race
           with a running vCPU), so survival is flat across modes.

Output: em_battery.png (300 dpi).
"""
import csv, os
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

HERE = os.path.dirname(os.path.abspath(__file__))
CSV = os.path.join(HERE, "em_results.csv")
PNG = os.path.join(HERE, "em_battery.png")

rows = list(csv.DictReader(open(CSV)))
batt = [r for r in rows if r["build"] in ("scalar", "avx")]
ctrl = [r for r in rows if r["build"] == "v1"]

CELLS = [("scalar", "(a) scalar poison: f=0.65 @ BI=1us (poll-storm family)"),
         ("avx", "(b) avx poison: f=0.25 @ BI=250ns (flip/flush family)")]
MODES = ("poll", "sleep", "doorbell")
COLORS = {"poll": "tab:red", "sleep": "tab:orange", "doorbell": "tab:blue"}

fig, axes = plt.subplots(1, 2, figsize=(7.2, 3.0), sharey=True)

for ax, (build, title) in zip(axes, CELLS):
    walls = []
    alive = []
    for mode in MODES:
        rr = [r for r in batt if r["build"] == build and r["notify"] == mode]
        ok = [r for r in rr if r["gate"] == "PASS"]
        alive.append(len(ok))
        ws = [float(r["wall_s"]) for r in ok]
        walls.append(np.mean(ws) if ws else 0.0)
    x = np.arange(3)
    ax.bar(x, alive, width=0.6, color=[COLORS[m] for m in MODES], alpha=0.85)
    for xi, (a, w) in enumerate(zip(alive, walls)):
        ax.text(xi, a + 0.06, "%d/3" % a, ha="center", fontsize=8.5)
        if w:
            ax.text(xi, -0.32, "%.1fs" % w, ha="center", fontsize=7,
                    color="0.25")
    ax.axhline(3, ls=":", lw=0.8, color="0.4")
    ax.set_xticks(x)
    ax.set_xticklabels(MODES, fontsize=8)
    ax.set_ylim(0, 3.7)
    ax.set_yticks([0, 1, 2, 3])
    ax.set_title(title, fontsize=8.5)
    ax.grid(alpha=0.3, lw=0.4, axis="y")

axes[0].set_ylabel("valid runs (byte-identical dump) / 3")

# CONTROL annotation on panel (a): v1 client vs the same v2 engine
if ctrl:
    c = ctrl[0]
    g = c["gate"]
    w = c["wall_s"]
    txt = "CTRL: v1 client vs v2 engine\n%s%s" % (
        g, (" %.1fs" % float(w)) if w else "")
    axes[0].text(0.02, 0.97, txt, transform=axes[0].transAxes,
                 va="top", ha="left", fontsize=7,
                 bbox=dict(fc="white", ec="0.6", alpha=0.9))

fig.tight_layout()
fig.savefig(PNG, dpi=300)
print("saved", PNG)
for build, _ in CELLS:
    for mode in MODES:
        rr = [r for r in batt if r["build"] == build and r["notify"] == mode]
        ok = [float(r["wall_s"]) for r in rr if r["gate"] == "PASS"]
        print("%-6s %-9s alive=%d/3 wall=%s" % (
            build, mode, len(ok), [("%.1f" % w) for w in ok]))
if ctrl:
    print("CTRL   v1@v2eng  gate=%s wall=%s" % (
        ctrl[0]["gate"], ctrl[0]["wall_s"]))
