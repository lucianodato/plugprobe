# plugprobe

Agent-driven audio plugin host: load any installed plugin (VST3/AU/LV2),
drive it like an end user through real OS mouse/keyboard, render audio
offline, diff the results. No DAW.

Single JUCE/C++ CLI, no new dependencies. Headless by
default (byte-identical renders); UI/visible/screenshot paths are opt-in.
OS driver + capture are macOS-only today; Windows/Linux backends are
compiled stubs that fail loud (`UNIMPLEMENTED_M1`).

## Commands

All commands: `plugprobe <cmd> --json <args.json>` →
`{ok, data|error}` on stdout, exit code 0/1.

| cmd | args → result |
|---|---|
| `scan` | `{paths?}` → installed plugins `[{id,file,format,params}]` |
| `inspect` | `{plugin}` → automatable `params[]` + `editor:{width,height,hasUI}` |
| `snapshot` | `{plugin, role?, limit?, offset?, shot?, visible?, holdMs?}` → AX node tree `[{id,role,name,enabled,value,bounds}]`. AX-empty custom-painted editors return containers only with `axEmpty:true` + fallback |
| `act` | `{plugin, via:"os"\|"juce", action\|actions, shotAfter?, visible?, holdMs?}` → detached probe click/drag/type. Node-id press works headless via AX; raw `{x,y}` clicks, drag, type need `visible:true` + the macOS Accessibility grant for the current binary (ad-hoc rebuilds invalidate it) |
| `render` | `{plugin, in, out, sr?, block?, params?, bypass?, timeline?, shot?, visible?, holdMs?}` → offline render. `timeline:[{atMs,params?,shot?,click?}]` runs on one instance for learn-freeze (per-entry live-editor shot + native click) |
| `compare` | `{a, b, slices?}` → `{nullDb,lufsDiff,spectralDist,sdrDb,artifactDb,costDelta}` |

`session start|act|stop` and `meters` are not implemented (`UNIMPLEMENTED_M2`);
they fail loud instead of pretending. The scripted equivalent is one
`render` call with a `timeline`.

Screenshots only on request (`shot`/`shotAfter`); blank (Metal/async)
captures are reported, never written. `visible:true` opens a real
on-screen window (Dock icon, steals focus — headed mode).

## Examples

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j4
BIN=$PWD/build/plugprobe_artefacts/Release/plugprobe

echo '{"plugin":"Example Plugin"}' > /tmp/insp.json
$BIN inspect --json /tmp/insp.json

# headed: snapshot with live window + screenshot
echo '{"plugin":"Example Denoiser","limit":50,"visible":true,"holdMs":500,"shot":"/tmp/shot.png"}' > /tmp/snap.json
$BIN snapshot --json /tmp/snap.json

# learn-freeze render: click the Learn button at t=0 on the rendering instance
echo '{"plugin":"Example Denoiser","in":"in.wav","out":"out.wav",
  "timeline":[{"atMs":0,"click":"AXButton:Learn"}]}' > /tmp/ren.json
$BIN render --json /tmp/ren.json

echo '{"a":"a.wav","b":"out.wav"}' > /tmp/cmp.json
$BIN compare --json /tmp/cmp.json
```

## Install

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4
cmake --install build --prefix ~/.local   # -> ~/.local/bin/plugprobe
```

Add `~/.local/bin` to `PATH` if needed. System-wide instead:

```sh
sudo cmake --install build --prefix /usr/local
```

## Tests

```sh
PLUGPROBE_BIN=$PWD/build/plugprobe_artefacts/Release/plugprobe \
  python3 tests/test_plugprobe.py   # stdlib only, 5 contract tests
```

`scripts/matrix.py` renders a directory of inputs through a plugin and
writes a per-file compare CSV against a reference plugin.

## Layout

```
src/plugprobe.cpp      CLI: scan/inspect/snapshot/act/render/compare
src/plugprobe_os.mm    macOS AX/CGEvent driver (snapshot dump, press, HID click/drag/type)
src/plugprobe_shot.mm  NSView capture + composited-window fallback + show-front
tests/test_plugprobe.py  contract tests (needs PLUGPROBE_BIN)
scripts/matrix.py      batch render+compare matrix
SPEC.md                full spec (v0.2, pre-rename; CLI contract in §3)
```

## License

GPL-3.0-or-later (see `LICENSE`). GPL fits: plugprobe links JUCE and
hosts VST3 plugins, so the distributed binary is GPL either way.
