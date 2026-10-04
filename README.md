# plugprobe

[![Build plugprobe](https://github.com/lucianodato/plugprobe/actions/workflows/build.yml/badge.svg)](https://github.com/lucianodato/plugprobe/actions/workflows/build.yml)

Agent-driven audio plugin host: load any installed plugin (VST3/AU/LV2),
drive it like an end user through real OS mouse/keyboard, render audio
offline, diff the results. No DAW.

Single JUCE/C++ CLI, no new dependencies. Headless by
default (byte-identical renders); UI/visible/screenshot paths are opt-in.
OS driver + capture are macOS-only today; Windows/Linux backends are
compiled stubs that fail loud (`NO_OS_DRIVER`).

Agents: read [SKILL.md](SKILL.md) — command patterns, RTFM workflow, error codes.

## Commands

All commands: `plugprobe <cmd> --json <args.json>` →
`{ok, data|error}` on stdout, exit code 0/1.

| cmd | args → result |
|---|---|
| `scan` | `{paths?, format?, cache?, rescan?}` → installed plugins `[{id,file,format,params}]` + `cached`. Opt-in `cache:path` memoizes the list (dir set + mtime validated); `PLUGPROBE_SCAN_CACHE` extends it to every command's plugin lookup |
| `inspect` | `{plugin}` → automatable `params[]` + `editor:{width,height,hasUI}` |
| `snapshot` | `{plugin, role?, limit?, offset?, shot?, visible?, holdMs?}` → AX node tree `[{id,role,name,enabled,value,bounds}]`. AX-empty custom-painted editors return containers only with `axEmpty:true` + fallback |
| `act` | `{plugin, via:"os"\|"juce", action\|actions, shotAfter?, visible?, holdMs?}` → detached probe click/drag/type. Node-id press works headless via AX; raw `{x,y}` clicks, drag, type need `visible:true` + the macOS Accessibility grant for the current binary (ad-hoc rebuilds invalidate it) |
| `render` | `{plugin, in, out, sr?, block?, params?, params_json?, midi?, midi_file?, bypass?, timeline?, shot?, visible?, holdMs?, video?}` → offline render. `params_json:preset.json` applies a `{"params":{...}}` preset file first (explicit `params` win). `midi:[{atMs,note,vel?,durMs?,ch?}]` / `midi_file:song.mid` (tempo-mapped) feeds instruments — silent `in` + notes is the synth smoke pattern. Every command runs crash-isolated: a dying plugin yields fail-loud `CRASH`, never a dead pipe. `timeline:[{atMs,params?,shot?,click?}]` runs on one instance for learn-freeze (per-entry live-editor shot + native click). `video:out.mp4` records the editor window headless (frame-grab, no grant, macOS-only) with the take muxed as audio; diagnostic-only (`video`/`videoSkipped`) — off-macOS the take still succeeds with `videoSkipped` |
| `compare` | `{a, b, slices?}` → `{nullDb,lufsDiff,spectralDist,sdrDb,artifactDb,costDelta}` |
| `session` | `start {plugin,loop,out,sr?,block?,params?,bypass?,visible?,dir?}` → `{session,outFile,startedAt,latencyMs}`; `act {session,params?|click?,at_ms?}` → `{atMs,paramDelta?,meters}`; `stop {session,out?}` → `{outFile,durS,eventLog,hash}`. Agent-paced file-backed takes (single loop pass, events aligned by `samplePos`); clicks verified read-only at act, pressed once at stop replay (macOS). Realtime loop + monitor mirror are the documented ceiling |
| `meters` | `{session,window_ms?}` → `{peakDb,rmsDb,lufsM,crestDb,spectrum[16]}` (lufsM is an RMS proxy, same convention as `compare`) |
| `manual` | `{plugin,paths?}` or `{path}` → `manuals:[{path,name,bytes}]` (bundled docs for the agent to read itself before clicking; empty + `note` when the vendor ships none) |

Screenshots only on request (`shot`/`shotAfter`); blank (Metal/async)
captures are reported, never written. `visible:true` opens a real
on-screen window (Dock icon, steals focus — headed mode).

## CI artifacts

Every output path is caller-chosen, so jobs upload exactly what was asked
for — no globs over temp dirs:

- `render`/`compare`/`snapshot`/`act`: pass absolute `out`/`shot` paths
  under one dir (e.g. `renders/${{ github.run_id }}/`).
- `session start`: pass `dir` (e.g. `renders/<date>/session`) to keep
  `session.json` next to the take; `stop` writes `outFile` + `eventLog`
  there. Without `dir` the session lives in system temp.
- `scripts/matrix.py --takes <dir>`: keeps rendered takes next to
  `matrix.csv` instead of system temp.

```yaml
- run: |
    BIN=build/plugprobe_artefacts/Release/plugprobe
    echo "{\"plugin\":\"Example Denoiser\",\"in\":\"in.wav\",\"out\":\"$GITHUB_WORKSPACE/renders/take.wav\",\"shot\":\"$GITHUB_WORKSPACE/renders/shot.png\",\"video\":\"$GITHUB_WORKSPACE/renders/take.mp4\"}" > /tmp/ren.json
    $BIN render --json /tmp/ren.json
- uses: actions/upload-artifact@v4
  with: {name: takes, path: renders/}
```

`video` is frame-grabbed headless (no grant) on macOS; without a working
encoder the take still succeeds with `videoSkipped`. Headless
Linux/Windows jobs omit `video` (`NO_OS_DRIVER`).

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
  python3 tests/test_plugprobe.py   # stdlib only, 13 contract tests
```

`scripts/matrix.py` renders a directory of inputs through a plugin and
writes a per-file compare CSV against a reference plugin.

`scripts/example_session.py` is the CI end-to-end proof: white noise through
the bundled example lowpass, cutoff swept 20kHz→1kHz via session acts, take
+ video (`sweep.mp4`) uploaded as artifacts.

## Layout

```
src/main.cpp            arg parsing, dispatch to cmd_*.cpp
src/core.cpp / core.h   emit, JSON args, hosting, params, WAV, metrics
src/gui.cpp / gui.h     live-editor open/hold/capture/hit-test
src/session.cpp / session.h  M2 session files, offline loop render, meters
src/cmds.h              one run function per CLI command
src/cmd_scan.cpp        scan installed plugins
src/cmd_inspect.cpp     params + editor size
src/cmd_snapshot.cpp    AX tree dump + opt-in shot
src/cmd_act.cpp         detached probe click/drag/type
src/cmd_render.cpp      offline render + learn-freeze timeline
src/cmd_compare.cpp     null/LUFS/spectral/SDR/cost diff
src/cmd_session.cpp     session start|act|stop + meters
src/plugprobe_os.h      OS-layer interface (front/capture/tree/input)
src/plugprobe_os_mac.mm macOS AX/CGEvent driver + NSView capture
src/plugprobe_os_win.cpp / plugprobe_os_linux.cpp  stub backends (fail loud)
tests/test_plugprobe.py  contract tests (needs PLUGPROBE_BIN)
scripts/matrix.py      batch render+compare matrix
scripts/example_session.py  CI end-to-end (noise -> example sweep -> take+video)
example/lowpass/       fixture plugin (smooth lowpass + spectrum + slider)
```

## License

GPL-3.0-or-later (see `LICENSE`). GPL fits: plugprobe links JUCE and
hosts VST3 plugins, so the distributed binary is GPL either way.
