#!/usr/bin/env python3
"""Plots from out/plot.json (written by tools/run.py): out/fade.png and out/faults.png."""
import json
import sys
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

OUT = Path(__file__).resolve().parent.parent / "out"
if not (OUT / "plot.json").exists():
    sys.exit("out/plot.json not found: run tools/run.py first (on Linux or in CI).")
d = json.loads((OUT / "plot.json").read_text())


def gamma(l):
    y = ((l + 16) / 116) ** 3 if l > 8 else l / 903.3
    return 100 * y


# 1. fade curves as the phone received them over BLE
fig, axes = plt.subplots(1, 2, figsize=(11, 3.8))
for ax, which, asked, start, end in ((axes[0], "up", 1000, 0, 100), (axes[1], "down", 3000, 100, 20)):
    pts = [(t, l) for w, t, l in d["fade"] if w == which]
    t = [0] + [p[0] for p in pts]
    lv = [start] + [p[1] for p in pts]
    ax.step(t, lv, where="post", lw=1.8, label="perceived level (status notifications)")
    ax.plot(t, [gamma(x) for x in lv], ".", ms=4, label="output duty after CIE 1931 gamma")
    ax.plot([0, asked], [start, end], "k--", lw=0.8, label=f"ideal {asked} ms linear fade")
    ax.set_title(f"fade {start} -> {end} %, asked {asked} ms")
    ax.set_xlabel("ms after the write")
    ax.set_ylabel("%")
    ax.grid(alpha=0.3)
axes[0].legend(fontsize=8, loc="lower right")
fig.tight_layout()
fig.savefig(OUT / "fade.png", dpi=130)

# 2. fault scenario timeline from the dimmer's 100 ms log
f = [x for x in d["faults"] if x[0] <= 5000]          # the central is done after ~4.2 s
t = [x[0] / 1000 for x in f]
fig, ax = plt.subplots(3, 1, figsize=(11, 6.5), sharex=True)
ax[0].plot(t, [x[1] / 10 for x in f], lw=1.5)
ax[0].set_ylabel("output duty %")
ax[1].plot(t, [x[2] / 1000 for x in f], lw=1.5, color="tab:orange")
ax[1].axhline(6.25, ls="--", lw=0.8, color="k")
ax[1].text(t[0], 6.4, "overload limit 6.25 A", fontsize=8)
ax[1].set_ylabel("load current A RMS")
ax[2].plot(t, [x[3] for x in f], lw=1.5, color="tab:red")
for y, lbl in ((80, "derate from 80 C"), (100, "trip 100 C")):
    ax[2].axhline(y, ls="--", lw=0.8, color="k")
    ax[2].text(t[0], y + 1, lbl, fontsize=8)
ax[2].set_ylabel("heatsink C")
ax[2].set_xlabel("simulated time s")
names = {1: "short", 2: "overload", 4: "overtemp"}
for a in ax:
    a.grid(alpha=0.3)
ax[0].set_title("fault scenario driven over BLE (dimmer log, 100 ms)\ntrips: "
                + ", ".join(f"{names.get(k, k)} {ms} ms" for k, ms in d["trips"]), fontsize=9)
fig.tight_layout()
fig.savefig(OUT / "faults.png", dpi=130)
print("wrote out/fade.png, out/faults.png")
sys.exit(0)
