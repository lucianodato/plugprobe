---
name: plugprobe
description: Drive audio plugins (VST3/AU/LV2) without a DAW — render audio, screenshot editors, click controls, run agent-paced sessions, compare takes. Use when asked to test, measure, automate, or screenshot an audio plugin, or when plugin UI control names need grounding.
---

# plugprobe

Agent-driven audio plugin host. CLI-only: every command is
`plugprobe <cmd> --json args.json` → one JSON object on stdout
(`{ok:true,data:{...}}` or `{ok:false,error:{code,message}}`, exit 1).

## Setup

- Binary: `$PLUGPROBE_BIN`, else `build/plugprobe_artefacts/Release/plugprobe`
  (`plugprobe.exe` on Windows). Never guess paths — ask the user or `which`.
- All file args accept absolute paths; keep them caller-chosen (CI artifacts).
- Repeat loops go faster with `PLUGPROBE_SCAN_CACHE=/tmp/scan.xml`
  (memoizes plugin discovery across commands, mtime-validated).

## Workflow (in order)

1. **RTFM**: `manual --json {plugin}` (or `{path}` to a vendor PDF/dir) →
   `manuals:[{path,name,bytes}]`. READ a manual yourself before clicking;
   empty list + `note` means the vendor ships none — proceed via `inspect`.
2. **Enumerate**: `scan --json {paths:[dir]}` → find the plugin;
   `inspect --json {plugin}` → automatable params; `snapshot --json {plugin}`
   → UI node ids (`role,name,bounds`).
3. **Act**: `act --json {plugin, via:"os"|"juce", action:{target,value}}`.
   Node-id press works headless; `set` drives sliders in native units
   (param-less controls, still grant-free); raw `{x,y}` clicks, drag, type
   need `"visible":true` + macOS Accessibility grant for the binary.
4. **Render**: `render --json {plugin,in,out,...}` → `{out,hash,peakDb}`.
   Presets via `params_json` (explicit `params` win); instruments via
   `midi:[{atMs,note,...}]` or tempo-mapped `midi_file` (silent `in` +
   notes is the synth smoke pattern). Every command runs
   crash-isolated: a dying plugin yields `CRASH`, never a dead pipe.
   `timeline:[{atMs,params?,click?}]` replays learn-freeze on one instance.
   Opt-in captures: `shot:path.png`, `video:out.mp4` (macOS, take muxed in).
5. **Session** (multi-step, agent-paced): `session start {loop,out,...}` →
   `session act {session,params?|click?}` (clicks verified read-only) →
   `session stop {session}` (presses once, renders). `meters {session}` anytime.
6. **Compare**: `compare --json {a,b}` → `{nullDb,lufsDiff,spectralDist,...}`.

## Rules

- Headless default is byte-identical; UI/visible/shot/video are opt-in only.
- Fail loud: `ARGS` (bad input), `NOT_FOUND` (no plugin), `NO_OS_DRIVER`
  (macOS-only op elsewhere), `CRASH` (plugin killed the host — rerun
  headless/bypass to isolate), `AX_UNTRUSTED` (grant missing). Never retry a
  fail silently — surface `error.code` + `message`.
- Captures are diagnostic-only: a dead recorder yields `shotSkipped` /
  `videoSkipped`, the take still succeeds.
- Bypass renders (`bypass:true`) process nothing — use for null-tests only.
