# GPL-3.0-or-later — Copyright (c) 2026 Luciano Dato — plugprobe
"""M0+M2+M3 contract tests, stdlib only. Needs PLUGPROBE_BIN (else skips buildable tests)."""
import json
import math
import os
import struct
import subprocess
import sys
import tempfile
import unittest
import wave

BIN = os.environ.get("PLUGPROBE_BIN", "")


def make_wav(path, secs=1.0, sr=48000, freq=440.0):
    n = int(secs * sr)
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(sr)
        for i in range(n):
            v = int(32767 * 0.5 * math.sin(2 * math.pi * freq * i / sr))
            w.writeframes(struct.pack("<h", v))
    return path


def run(cmd, args, env=None):
    with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False) as f:
        json.dump(args, f)
        p = f.name
    try:
        e = dict(os.environ)
        if env:
            e.update(env)
        r = subprocess.run([BIN, cmd, "--json", p], capture_output=True,
                           text=True, timeout=120, env=e)
        return json.loads(r.stdout.strip().splitlines()[-1]), r.returncode
    finally:
        os.unlink(p)


class Contract(unittest.TestCase):
    def test_help_version(self):
        if not BIN:
            self.skipTest("PLUGPROBE_BIN unset")
        for flag in ("--help", "--version"):
            r = subprocess.run([BIN, flag], capture_output=True, text=True,
                               timeout=30)
            self.assertEqual(r.returncode, 0)
            self.assertTrue(r.stdout.strip())
        self.assertIn("plugprobe", subprocess.run(
            [BIN, "--version"], capture_output=True, text=True,
            timeout=30).stdout)

    def test_json_inline_and_stdin(self):
        if not BIN:
            self.skipTest("PLUGPROBE_BIN unset")
        d = tempfile.mkdtemp()
        inline = subprocess.run(
            [BIN, "scan", "--json", json.dumps({"paths": [d]})],
            capture_output=True, text=True, timeout=60)
        self.assertTrue(json.loads(inline.stdout.strip().splitlines()[-1])["ok"])
        piped = subprocess.run(
            [BIN, "scan", "--json", "-"], input=json.dumps({"paths": [d]}),
            capture_output=True, text=True, timeout=60)
        self.assertTrue(json.loads(piped.stdout.strip().splitlines()[-1])["ok"])

    def test_unknown_cmd_shape(self):
        if not BIN:
            self.skipTest("PLUGPROBE_BIN unset")
        out, rc = run("nope", {})
        self.assertFalse(out["ok"])
        self.assertIn("code", out["error"])
        self.assertEqual(rc, 1)

    def test_scan_empty_dir(self):
        if not BIN:
            self.skipTest("PLUGPROBE_BIN unset")
        d = tempfile.mkdtemp()
        out, _ = run("scan", {"paths": [d]})
        self.assertTrue(out["ok"])
        self.assertIn("plugins", out["data"])

    def test_bypass_null_and_determinism(self):
        if not BIN:
            self.skipTest("PLUGPROBE_BIN unset")
        tmp = tempfile.mkdtemp()
        src = make_wav(os.path.join(tmp, "in.wav"))
        o1, o2 = os.path.join(tmp, "o1.wav"), os.path.join(tmp, "o2.wav")
        r1, _ = run("render", {"plugin": "none", "in": src, "out": o1,
                               "bypass": True, "tail_ms": 0})
        r2, _ = run("render", {"plugin": "none", "in": src, "out": o2,
                               "bypass": True, "tail_ms": 0})
        self.assertTrue(r1["ok"] and r2["ok"])
        self.assertEqual(r1["data"]["hash"], r2["data"]["hash"])
        c, _ = run("compare", {"a": src, "b": o1})
        self.assertTrue(c["ok"])
        self.assertLess(c["data"]["nullDb"], -60.0)  # bypass must cancel
        self.assertAlmostEqual(c["data"]["lufsDiff"], 0.0, places=1)

    def test_compare_identical(self):
        if not BIN:
            self.skipTest("PLUGPROBE_BIN unset")
        tmp = tempfile.mkdtemp()
        src = make_wav(os.path.join(tmp, "in.wav"))
        c, _ = run("compare", {"a": src, "b": src})
        self.assertTrue(c["ok"])
        self.assertLess(c["data"]["nullDb"], -60.0)

    def test_session_lifecycle(self):
        if not BIN:
            self.skipTest("PLUGPROBE_BIN unset")
        tmp = tempfile.mkdtemp()
        src = make_wav(os.path.join(tmp, "in.wav"))
        take = os.path.join(tmp, "take.wav")
        s, _ = run("session-start", {"loop": src, "out": take,
                                     "bypass": True})
        self.assertTrue(s["ok"])
        ses = s["data"]["session"]
        m, _ = run("meters", {"session": ses, "window_ms": 500})
        self.assertTrue(m["ok"])
        for k in ("peakDb", "rmsDb", "lufsM", "crestDb", "spectrum"):
            self.assertIn(k, m["data"])
        a, _ = run("session-act", {"session": ses, "params": {"0": 0.5}})
        self.assertEqual(a["error"]["code"], "ARGS")  # bypass: no params
        st, _ = run("session-stop", {"session": ses})
        self.assertTrue(st["ok"])
        self.assertEqual(st["data"]["eventLog"], [])
        self.assertTrue(os.path.exists(take))
        c, _ = run("compare", {"a": src, "b": take})
        self.assertLess(c["data"]["nullDb"], -60.0)  # passthrough take

    def test_video_skipped_without_instance(self):
        if not BIN:
            self.skipTest("PLUGPROBE_BIN unset")
        tmp = tempfile.mkdtemp()
        src = make_wav(os.path.join(tmp, "in.wav"))
        o = os.path.join(tmp, "o.wav")
        r, _ = run("render", {"plugin": "none", "in": src, "out": o,
                              "bypass": True, "tail_ms": 0,
                              "video": os.path.join(tmp, "v.mp4")})
        self.assertTrue(r["ok"])
        self.assertEqual(r["data"]["videoSkipped"], "bypass-no-instance")

    def test_manual_locates_docs(self):
        if not BIN:
            self.skipTest("PLUGPROBE_BIN unset")
        tmp = tempfile.mkdtemp()
        pdf = os.path.join(tmp, "UserManual.pdf")
        open(pdf, "wb").write(b"%PDF-1.4 fake")
        open(os.path.join(tmp, "noise.wav"), "wb").write(b"RIFF")
        r, _ = run("manual", {"path": pdf})
        self.assertTrue(r["ok"])
        self.assertEqual(r["data"]["manuals"][0]["name"], "UserManual.pdf")
        r, _ = run("manual", {"path": tmp})
        self.assertTrue(r["ok"])
        self.assertEqual(len(r["data"]["manuals"]), 1)  # wav ignored
        r, _ = run("manual", {"path": os.path.join(tmp, "nope.pdf")})
        self.assertEqual(r["error"]["code"], "ARGS")
        r, _ = run("manual", {"plugin": "no-such-plugin-xyz-123"})
        self.assertEqual(r["error"]["code"], "NOT_FOUND")

    def test_crash_isolation(self):
        if not BIN:
            self.skipTest("PLUGPROBE_BIN unset")
        out, rc = run("nope", {}, env={"PLUGPROBE_INJECT_CRASH": "1"})
        self.assertFalse(out["ok"])
        self.assertEqual(out["error"]["code"], "CRASH")
        self.assertEqual(rc, 1)

    def test_preset_fail_loud(self):
        if not BIN:
            self.skipTest("PLUGPROBE_BIN unset")
        tmp = tempfile.mkdtemp()
        src = make_wav(os.path.join(tmp, "in.wav"))
        o = os.path.join(tmp, "o.wav")
        base = {"plugin": "none", "in": src, "out": o,
                "bypass": True, "tail_ms": 0}
        r, _ = run("render", dict(base, params_json=os.path.join(tmp, "no.json")))
        self.assertEqual(r["error"]["code"], "ARGS")  # missing file
        bad = os.path.join(tmp, "bad.json")
        open(bad, "w").write("{not json")
        r, _ = run("render", dict(base, params_json=bad))
        self.assertEqual(r["error"]["code"], "ARGS")  # malformed
        arr = os.path.join(tmp, "arr.json")
        open(arr, "w").write("[1,2]")
        r, _ = run("render", dict(base, params_json=arr))
        self.assertEqual(r["error"]["code"], "ARGS")  # needs map

    def test_scan_cache(self):
        if not BIN:
            self.skipTest("PLUGPROBE_BIN unset")
        tmp = tempfile.mkdtemp()
        d = os.path.join(tmp, "plugs")
        os.mkdir(d)
        c = os.path.join(tmp, "scan.xml")
        r, _ = run("scan", {"paths": [d], "cache": c})
        self.assertTrue(r["ok"])
        self.assertFalse(r["data"]["cached"])
        self.assertTrue(os.path.exists(c))
        r, _ = run("scan", {"paths": [d], "cache": c})
        self.assertTrue(r["data"]["cached"])
        open(os.path.join(d, "new.txt"), "w").write("x")
        r, _ = run("scan", {"paths": [d], "cache": c})
        self.assertFalse(r["data"]["cached"])  # mtime invalidated
        r, _ = run("scan", {"paths": [d], "cache": c, "rescan": True})
        self.assertFalse(r["data"]["cached"])
        # findPlugin honors the env cache too (populates on miss)
        c2 = os.path.join(tmp, "env.xml")
        src2 = make_wav(os.path.join(tmp, "in2.wav"))
        r, _ = run("render", {"plugin": "no-such-plugin-xyz-123",
                              "in": src2,
                              "out": os.path.join(tmp, "y.wav")},
                   env={"PLUGPROBE_SCAN_CACHE": c2})
        self.assertEqual(r["error"]["code"], "NOT_FOUND")
        self.assertTrue(os.path.exists(c2))

    def test_midi_validation(self):
        if not BIN:
            self.skipTest("PLUGPROBE_BIN unset")
        tmp = tempfile.mkdtemp()
        src = make_wav(os.path.join(tmp, "in.wav"))
        o = os.path.join(tmp, "o.wav")
        base = {"plugin": "none", "in": src, "out": o,
                "bypass": True, "tail_ms": 0}
        good = {"atMs": 100, "note": 69}
        for bad in ({"atMs": -1, "note": 69}, {"atMs": 0, "note": 128},
                    {"atMs": 0, "note": 60, "vel": 0},
                    {"atMs": 0, "note": 60, "ch": 17},
                    {"atMs": 0, "note": 60, "durMs": 0},
                    {"atMs": 5000, "note": 60}):  # past 1s render
            r, _ = run("render", dict(base, midi=[bad]))
            self.assertEqual(r["error"]["code"], "ARGS", bad)
        r, _ = run("render", dict(base, midi={"atMs": 0, "note": 60}))
        self.assertEqual(r["error"]["code"], "ARGS")  # needs array
        r, _ = run("render", dict(base, midi_file=os.path.join(tmp, "no.mid")))
        self.assertEqual(r["error"]["code"], "ARGS")
        badf = os.path.join(tmp, "bad.mid")
        open(badf, "wb").write(b"not a midi file")
        r, _ = run("render", dict(base, midi_file=badf))
        self.assertEqual(r["error"]["code"], "ARGS")
        r, _ = run("render", dict(base, midi=[good]))
        self.assertTrue(r["ok"])
        self.assertEqual(r["data"]["midiEvents"], 2.0)  # on + off

    def test_midi_file(self):
        if not BIN:
            self.skipTest("PLUGPROBE_BIN unset")
        tmp = tempfile.mkdtemp()
        src = make_wav(os.path.join(tmp, "in.wav"))
        # minimal type-0 .mid: prog change, C4 on, C4 off 72 ticks later
        trk = bytes.fromhex("00c000") + bytes.fromhex("00903c64") + \
            bytes.fromhex("48803c40")
        mid = b"MThd" + struct.pack(">IHHH", 6, 0, 1, 480) + \
            b"MTrk" + struct.pack(">I", len(trk)) + trk
        mf = os.path.join(tmp, "n.mid")
        open(mf, "wb").write(mid)
        a = {"plugin": "none", "in": src, "out": os.path.join(tmp, "o.wav"),
             "bypass": True, "tail_ms": 0, "midi_file": mf}
        r1, _ = run("render", a)
        self.assertTrue(r1["ok"])
        self.assertEqual(r1["data"]["midiEvents"], 3.0)
        a["out"] = os.path.join(tmp, "o2.wav")
        r2, _ = run("render", a)
        self.assertEqual(r1["data"]["hash"], r2["data"]["hash"])

    def test_session_slider_args(self):
        if not BIN:
            self.skipTest("PLUGPROBE_BIN unset")
        tmp = tempfile.mkdtemp()
        src = make_wav(os.path.join(tmp, "in.wav"))
        take = os.path.join(tmp, "take.wav")
        s, _ = run("session-start", {"loop": src, "out": take,
                                     "bypass": True})
        self.assertTrue(s["ok"])
        ses = s["data"]["session"]
        a, _ = run("session-act", {"session": ses,
                                   "slider": {"target": "X", "value": 1}})
        self.assertEqual(a["error"]["code"], "ARGS")  # bypass: no editor
        for bad in ({"target": "X"}, {"target": "", "value": 1},
                    {"target": "X", "value": "hi"}, "nope"):
            a, _ = run("session-act", {"session": ses, "slider": bad})
            self.assertEqual(a["error"]["code"], "ARGS", bad)
        st, _ = run("session-stop", {"session": ses})
        self.assertTrue(st["ok"])

    def test_stubs_fail_loudly(self):
        if not BIN:
            self.skipTest("PLUGPROBE_BIN unset")
        out, rc = run("act", {"plugin": "no-such-plugin-xyz-123", "via": "os",
                              "action": {"target": "AXButton:Learn"}})
        self.assertFalse(out["ok"])
        self.assertEqual(out["error"]["code"], "NOT_FOUND")
        out, _ = run("act", {"plugin": "no-such-plugin-xyz-123", "via": "os",
                             "action": {"click": 1}})
        self.assertEqual(out["error"]["code"], "ARGS")
        out, _ = run("act", {"plugin": "no-such-plugin-xyz-123", "via": "os",
                             "action": {"target": {"a": 1}}})
        self.assertEqual(out["error"]["code"], "ARGS")  # object needs {x,y}
        out, _ = run("session-start", {"loop": "nope.wav"})
        self.assertEqual(out["error"]["code"], "ARGS")  # needs loop+out
        out, _ = run("session-stop", {"session": "/no-such-session"})
        self.assertEqual(out["error"]["code"], "ARGS")
        s, _ = run("snapshot", {"plugin": "no-such-plugin-xyz-123",
                                "limit": 10, "offset": 0})
        self.assertFalse(s["ok"])
        self.assertEqual(s["error"]["code"], "NOT_FOUND")
        s, _ = run("snapshot", {"limit": 10, "offset": 0})
        self.assertEqual(s["error"]["code"], "ARGS")

    def test_os_driver_gate_off_macos(self):
        plugin = os.environ.get("PLUGPROBE_TEST_PLUGIN", "")
        if not BIN or not plugin or sys.platform == "darwin":
            self.skipTest("needs PLUGPROBE_TEST_PLUGIN on a non-macOS host")
        s, _ = run("snapshot", {"plugin": plugin, "limit": 10})
        self.assertEqual(s["error"]["code"], "NO_OS_DRIVER")
        out, _ = run("act", {"plugin": plugin, "via": "os",
                             "action": {"target": "AXButton:Learn"}})
        self.assertEqual(out["error"]["code"], "NO_OS_DRIVER")


if __name__ == "__main__":
    unittest.main()
