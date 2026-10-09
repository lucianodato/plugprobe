#!/usr/bin/env python3
# GPL-3.0-or-later — Copyright (c) 2026 Luciano Dato — plugprobe
"""End-to-end proof: white noise -> example lowpass -> cutoff sweep session.
Sweeps Cutoff 20kHz..1kHz via session acts (explicit at_ms, wall-clock
independent), stops, and asserts the take's SPECTRUM moved (nullDb is
sample-wise: filter phase shift alone fools it) plus the video artifact.
Usage: example_session.py --bin BIN --plugin PLUG.vst3 --out DIR
       [--require-video]  (video is macOS-only; elsewhere audio-only proof)
"""
import argparse
import json
import os
import random
import struct
import subprocess
import sys
import tempfile
import wave


def run(bin_, cmd, args):
    with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False) as f:
        json.dump(args, f)
        p = f.name
    try:
        r = subprocess.run([bin_, cmd, "--json", p], capture_output=True,
                           text=True, timeout=600)
        return json.loads(r.stdout.strip().splitlines()[-1])
    finally:
        os.unlink(p)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", required=True)
    ap.add_argument("--plugin", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--require-video", action="store_true")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    random.seed(1234)

    loop = os.path.join(a.out, "loop.wav")
    with wave.open(loop, "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(48000)
        w.writeframes(b"".join(struct.pack("<h", random.randint(-16000, 16000))
                               for _ in range(48000 * 4 * 2)))
    take = os.path.join(a.out, "take.wav")
    vid = os.path.join(a.out, "sweep.avi")

    s = run(a.bin, "session-start", {"plugin": a.plugin, "loop": loop,
                                     "out": take, "video": vid})
    assert s["ok"], s
    ses = s["data"]["session"]
    for i, hz in enumerate([20000, 10000, 5000, 2000, 1000]):
        norm = (hz - 20) / 19980  # example plugin range is linear 20..20000
        r = run(a.bin, "session-act",
                {"session": ses, "at_ms": 500 * (i + 1),
                 "params": {"Cutoff": norm}})
        assert r["ok"], (hz, r)
    # Param-less Trim is driven through each OS's accessibility tree.
    mac = sys.platform == "darwin"
    windows = sys.platform == "win32"
    trim_target = "AXSlider:Trim" if mac else "Slider:Trim"
    trim_ev = {"target": trim_target, "value": 0.5}
    # Hosted runners may not expose editor controls; probe before asserting.
    has_trim = False
    if mac or windows:
        probe = run(a.bin, "snapshot", {"plugin": a.plugin, "limit": 500})
        ids = ([n.get("id") for n in probe.get("data", {}).get("nodes", [])]
               if probe.get("ok") else [])
        has_trim = trim_target in ids
        if not has_trim:
            print(f"SKIP_UI no Trim slider in this session (nodes={len(ids)})")
    if has_trim:
        # Detached first, then replayed in the take.
        d = run(a.bin, "act", {"plugin": a.plugin, "via": "os",
                               "action": {"target": trim_target,
                                           "op": "set", "value": 1.5}})
        assert d["ok"], d
        assert abs(d["data"]["results"][0]["state"] - 1.5) < 1e-6, d
        for ms, v in ((3000, 0.5), (3500, 2.0)):
            r = run(a.bin, "session-act",
                    {"session": ses, "at_ms": ms,
                     "slider": {"target": trim_target, "value": v}})
            assert r["ok"], (ms, v, r)
    elif not (mac or windows):
        r = run(a.bin, "act", {"plugin": a.plugin, "via": "os",
                               "action": {"target": trim_target,
                                           "op": "set", "value": 1.5}})
        assert r["error"]["code"] == "NO_OS_DRIVER", r
        r = run(a.bin, "session-act",
                {"session": ses, "at_ms": 3000, "slider": trim_ev})
        assert r["error"]["code"] == "NO_OS_DRIVER", r
    st = run(a.bin, "session-stop", {"session": ses})
    assert st["ok"], st
    if has_trim:
        log = st["data"]["eventLog"]
        assert any(e.get("slider", {}).get("target") == trim_target
                   for e in log), log
        # Audio proof the replayed set lands: mute the whole take via timeline.
        mute = run(a.bin, "render",
                   {"plugin": a.plugin, "in": loop,
                     "out": os.path.join(a.out, "mute.wav"), "tail_ms": 0,
                     "timeline": [{"atMs": 0,
                                   "slider": {"target": trim_target,
                                              "value": 0.0}}]})
        assert mute["ok"], mute
        assert mute["data"]["peakDb"] <= -100, mute  # default trim=1 is loud
    elif not (mac or windows):
        r = run(a.bin, "render",
                {"plugin": a.plugin, "in": loop,
                 "out": os.path.join(a.out, "mute.wav"), "tail_ms": 0,
                 "timeline": [{"atMs": 0, "slider": trim_ev}]})
        assert r["error"]["code"] == "NO_OS_DRIVER", r

    c = run(a.bin, "compare", {"a": loop, "b": take})
    assert c["ok"], c
    sd = c["data"]["spectralDist"]
    assert sd > 1.0, f"spectrum did not move (spectralDist={sd})"
    assert os.path.getsize(take) > 100000, "take too small"
    if a.require_video:
        assert os.path.exists(vid), "no video artifact"
        blob = open(vid, "rb").read()
        assert len(blob) > 50000, f"video too small ({len(blob)})"
        assert b"vids" in blob and b"auds" in blob, "video lacks A/V tracks"
        avih = blob.find(b"avih")
        assert avih >= 0 and avih + 28 <= len(blob), "video lacks AVI header"
        frame_us = struct.unpack_from("<I", blob, avih + 8)[0]
        frames = struct.unpack_from("<I", blob, avih + 24)[0]
        with wave.open(take, "rb") as audio:
            audio_secs = audio.getnframes() / audio.getframerate()
        video_secs = frame_us * frames / 1_000_000
        assert abs(video_secs - audio_secs) <= frame_us / 1_000_000, (
            f"A/V duration mismatch ({video_secs:.2f}s video, "
            f"{audio_secs:.2f}s audio)")
    print(f"EXAMPLE_OK spectralDist={sd:.2f} take={take}"
          + (f" video={vid}" if a.require_video else ""))


if __name__ == "__main__":
    sys.exit(main())
