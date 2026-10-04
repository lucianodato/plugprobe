#!/usr/bin/env python3
# GPL-3.0-or-later — Copyright (c) 2026 Luciano Dato — plugprobe
"""matrix.py: scripted A/B over shared fixtures. render each input through
each --plugin, compare vs ref (or input for null sanity), write matrix.csv.
Usage: matrix.py --plugin <id|path> [--ref <id|path|none>] --inputs <dir>
  --out matrix.csv [--takes takes/ --sr 48000 --block 512 --params_json p.json]"""
import argparse
import csv
import json
import os
import subprocess
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)


def plugprobe(cmd, args, bin):
    with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False) as f:
        json.dump(args, f)
        path = f.name
    try:
        p = subprocess.run([bin, cmd, "--json", path], capture_output=True,
                           text=True, timeout=300)
        return json.loads(p.stdout.strip().splitlines()[-1])
    finally:
        os.unlink(path)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--plugin", required=True)
    ap.add_argument("--ref", default="none")
    ap.add_argument("--inputs", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--takes", default="",
                    help="dir for rendered takes (uploadable artifacts); "
                         "default: system temp (takes not kept)")
    ap.add_argument("--bin", default=os.environ.get("PLUGPROBE_BIN", "plugprobe"))
    ap.add_argument("--sr", type=float, default=48000)
    ap.add_argument("--block", type=int, default=512)
    ap.add_argument("--params_json", default="")
    ap.add_argument("--bypass", action="store_true")
    a = ap.parse_args()

    wavs = sorted(f for f in os.listdir(a.inputs) if f.endswith(".wav"))
    os.makedirs(os.path.dirname(os.path.abspath(a.out)) or ".", exist_ok=True)
    takes_dir = a.takes or tempfile.gettempdir()
    if a.takes:
        os.makedirs(takes_dir, exist_ok=True)
    with open(a.out, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=["file", "plugin", "hash", "nullDb",
                                          "lufsDiff", "spectralDist", "sdrDb",
                                          "artifactDb", "costDelta"])
        w.writeheader()
        for name in wavs:
            src = os.path.join(a.inputs, name)
            take = os.path.join(takes_dir, "plugprobe-" + name)
            rargs = {"plugin": a.plugin, "in": src, "out": take,
                     "sr": a.sr, "block": a.block, "bypass": a.bypass}
            if a.params_json:
                rargs["params_json"] = a.params_json
            r = plugprobe("render", rargs, a.bin)
            if not r.get("ok"):
                w.writerow({"file": name, "plugin": a.plugin, "hash": "RENDER_FAIL",
                            "nullDb": "", "lufsDiff": "", "spectralDist": "",
                            "sdrDb": "", "artifactDb": "",
                            "costDelta": json.dumps(r.get("error", {}))[:80]})
                continue
            base = src
            if a.ref != "none":
                ref_take = os.path.join(takes_dir, "plugprobe-ref-" + name)
                rr = plugprobe("render", {"plugin": a.ref, "in": src, "out": ref_take,
                                      "sr": a.sr, "block": a.block}, a.bin)
                if rr.get("ok"):
                    base = ref_take
            c = plugprobe("compare", {"a": base, "b": take}, a.bin)
            d = c.get("data", {}) if c.get("ok") else {}
            w.writerow({"file": name, "plugin": a.plugin,
                        "hash": r["data"].get("hash", ""),
                        "nullDb": d.get("nullDb", ""), "lufsDiff": d.get("lufsDiff", ""),
                        "spectralDist": d.get("spectralDist", ""), "sdrDb": d.get("sdrDb", ""),
                        "artifactDb": d.get("artifactDb", ""), "costDelta": d.get("costDelta", "")})
    print("wrote " + a.out)


if __name__ == "__main__":
    main()
