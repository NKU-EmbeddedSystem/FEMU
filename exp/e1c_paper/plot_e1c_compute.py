#!/usr/bin/env python3
"""plot_e1c_compute.py: device-compute sensitivity (Phase C E1').

x = per-dist compute busy-wait (ns) at the wiki 21M f=0.5 collab point
(48GB device, default profile). Real-FPGA range 2-8 TFLOPS = 0.19-0.77
ns/dist (knob 0 = default profile); knee at ~44us/dist (~0.032mT).
"""
import csv
import os

import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

HERE = os.path.dirname(os.path.abspath(__file__))
CSV = os.path.join(HERE, "e1c_compute_sensitivity.csv")
PNG = os.path.join(HERE, "e1c_compute_sensitivity.png")

rows = [r for r in csv.DictReader(open(CSV)) if r["wall_s"]]
v = np.array([float(r["comp_ns"]) for r in rows])
wall = np.array([float(r["wall_s"]) for r in rows])
qps = np.array([float(r["qps"]) for r in rows])

flat = wall[v <= 256]
wall0 = float(flat.mean())
slope = 500.0 * 4726.7 * 1e-9   # s per ns-knob
grid = np.logspace(-0.1, 5.35, 300)
qps_model = 1000.0 / (wall0 + slope * grid)
v_star = (217.4 - wall0) / slope

fig, ax = plt.subplots(figsize=(4.8, 3.2))

ax.axvspan(0.15, 0.8, color="tab:blue", alpha=0.12, lw=0)
ax.text(0.17, 5.9, "2-8 TFLOPS\nFPGA (knob=0)", fontsize=6.5,
        color="tab:blue", ha="left")

mv = v[v > 0]
ax.plot(mv, qps[v > 0], "o", color="tab:blue", ms=5, label="measured")
ax.plot([0.5], [qps[v == 0][0]], "*", color="tab:red", ms=11,
        label="default profile (2-8T)")
ax.plot(grid, qps_model, "--", color="0.55", lw=1.1,
        label="model: 1000/(wall0 + 500*dist*v)")
ax.axhline(4.6, ls=":", lw=0.9, color="0.4")
ax.text(2.0, 4.75, "pure CPU avx 4.6 QPS", fontsize=6.5, color="0.35")
ax.axvline(v_star, ls=":", lw=0.9, color="tab:red")
ax.text(v_star * 1.15, 6.6,
        "knee: collab ties\npure CPU at %.0f us" % (v_star / 1000.0),
        fontsize=6.5, color="tab:red")

# ARM-controller anchor (8.9.7 weak-controller axis)
i_arm = np.where(v == 1000.0)[0]
if len(i_arm):
    ax.plot([v[i_arm[0]]], [qps[i_arm[0]]], "^", color="tab:purple", ms=6,
            label="ARM ctrl 1000ns")

ax.set_xscale("log")
ax.set_xlim(0.35, 3e5)
ax.set_ylim(3.8, 9.8)
ax.set_xlabel("device compute, per-dist busy-wait (ns)")
ax.set_ylabel("collab throughput (QPS)")

# TFLOPS equivalents annotated at the key knots (secondary-axis log
# label placement collided with matplotlib's default log ticks)
ax.text(1.0, 9.35, "1.5T", fontsize=6.5, color="0.4", ha="left")
ax.text(64, 9.35, "0.024T", fontsize=6.5, color="0.4", ha="left")
ax.text(4300, 9.35, "0.36mT", fontsize=6.5, color="0.4", ha="left")
ax.text(60000, 9.35, "32uT (knee)", fontsize=6.5, color="0.4", ha="left")

ax.grid(alpha=0.3, lw=0.4)
for s in ("top", "right"):
    ax.spines[s].set_visible(False)
ax.tick_params(labelsize=8)
ax.legend(fontsize=6.5, frameon=False, loc="upper right")

fig.tight_layout()
fig.savefig(PNG, dpi=300)
print("saved", PNG)
print("wall0=%.1f slope=%.4g s/ns v*=%.0f ns (%.1f us)" %
      (wall0, slope, v_star, v_star / 1000.0))
