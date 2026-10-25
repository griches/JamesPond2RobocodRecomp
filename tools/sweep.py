#!/usr/bin/env python3
"""Visit every map section with the game's own cheat and record any code the
recompiled build had to interpret. The results are merged into one coverage
file the recompiler reads on the next build.

usage: sweep.py <jp2 binary> <adf> <out coverage .bin> [first last]
"""
import os
import subprocess
import sys
import tempfile

jp2, adf, out = sys.argv[1], sys.argv[2], sys.argv[3]
first, last = (int(sys.argv[4]), int(sys.argv[5])) if len(sys.argv) > 5 else (0, 60)

# Into the game, cheat on, map select, section, Return. Then invincibility,
# a mix of running, jumping and the vehicles, and finally advance to exit.
BOOT = "450-456:fire,700-706:fire,900-906:fire,1100-1106:fire,1250:type=little mermaid,1350-1352:key=M"


def script(section):
    s = [BOOT, f"1380:type={section}", "1420-1422:key=RETURN", "1700-1702:key=RETURN"]
    t = 1720
    for i in range(10):
        s.append(f"{t}-{t + 60}:right+fire" if i % 2 == 0 else f"{t}-{t + 40}:left")
        s.append(f"{t + 20}-{t + 26}:up")
        t += 70
    for k in "FPBC":
        s.append(f"{t}-{t + 2}:key={k}")
        s.append(f"{t + 10}-{t + 90}:right+fire")
        t += 100
    s.append(f"{t}-{t + 2}:key=X")
    s.append(f"{t + 60}-{t + 160}:right+fire")
    return ",".join(s), t + 400


merged = bytearray(0x80000)
missed = {}
with tempfile.TemporaryDirectory() as tmp:
    for sec in range(first, last + 1):
        spec, frames = script(sec)
        rec = os.path.join(tmp, f"s{sec}")
        os.makedirs(rec)
        r = subprocess.run([jp2, "--adf", adf, "--headless", str(frames), "--input", spec, "--record", rec],
                           capture_output=True, text=True)
        cov = open(os.path.join(rec, "coverage.bin"), "rb").read()
        n = 0
        for i, b in enumerate(cov):
            if b:
                n += 1
                merged[i] = 1
        missed[sec] = n
        print(f"section {sec:3d}: {n:4d} interpreted instruction addresses", flush=True)

total = sum(1 for b in merged if b)
os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
if os.path.exists(out):
    old = open(out, "rb").read()
    for i, b in enumerate(old):
        if b:
            merged[i] = 1
open(out, "wb").write(bytes(merged))
print(f"{total} interpreted addresses this sweep; merged into {out}")
