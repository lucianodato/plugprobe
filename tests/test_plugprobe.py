# GPL-3.0-or-later — Copyright (c) 2026 Luciano Dato — plugprobe
"""M0+M2+M3 contract tests, stdlib only. Needs PLUGPROBE_BIN (else skips buildable tests)."""
import json
import math
import os
import struct
import subprocess
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


def run(cmd, args):
    with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False) as f:
        json.dump(args, f)
        p = f.name
    try:
        r = subprocess.run([BIN, cmd, "--json", p], capture_output=True,
                           text=True, timeout=120)
        return json.loads(r.stdout.strip().splitlines()[-1]), r.returncode
    finally:
        os.unlink(p)


class Contract(unittest.TestCase):
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


if __name__ == "__main__":
    unittest.main()
