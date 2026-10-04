# plugprobe — Spec v0.2

Generic agent-driven plugin host: load any installed audio plugin
(JUCE or not), drive it like an end user through real OS mouse/keyboard,
tweak it live while audio runs, capture the result, diff inputs vs
outputs. Scripted, click-driven plugin testing — no DAW.

## 0. Feasibility verdict: YES, in three layers

- **Params/render (easy, deterministic):** JUCE `AudioPluginFormatManager`
  hosts VST3/AU/LV2, enumerates automatable params, renders WAV files
  offline. No DAW. Covers regression checks, not realism.
- **OS input driver (generic UI path):** synthetic OS-level mouse/keyboard —
  macOS CGEvent, Windows SendInput, Linux XTest — clicks the real editor
  window like a user. Works for ANY plugin framework (JUCE or proprietary),
  including non-automatable controls (Learn buttons) that
  params can never reach. Proven in miniature by
  `rx-ab-study/host/click.swift`; this spec promotes that hack to a proper
  cross-platform driver with window focus, node→screen mapping, and action
  logging. Requires one-time OS grants (Accessibility on macOS, UIA on
  Windows) — documented, fail loudly without them.
- **Live session (realism path):** real-time looped playback through the
  plugin while the agent acts, recording output + meter/spectrum taps so
  the agent gets context on what each tweak does to the sound.
- **JUCE component tree (enhancement, JUCE plugins only):** semantic
  snapshot for stable selectors; all other plugins use the AX/UIA tree or
  screenshot + coordinates.

Rule: params are ground truth for assertions, OS actions are the realism
mechanism, and every OS action must still report `paramDelta` when one
exists (flag `uiOnly:true` otherwise, e.g. Learn). Never assert on pixels
alone.

## 1. Goals / non-goals

Goals: file in → processed file out for any VST3/AU/LV2 regardless of
framework; list/get/set params; snapshot editor UI as JSON; OS-level
click/drag/scroll/type by node id or screen coords; screenshot; offline
render with fixed config; live tweak-and-listen sessions (loop input,
record output, meter/spectrum feedback while acting); A/B + input-vs-output
compare suite in the vein of rx-ab-study (`compare.py`, `delta.py`,
`metrics.py`, `speech_cost.py`): null depth, LUFS diff, spectral distance,
SDR, artifact residual, task cost.
Non-goals: DAW project hosting (no REAPER), plugin validation, license
cracking, mobile. Real-time auditioning is IN scope via live sessions,
not via a DAW.

## 2. Architecture (one binary + thin bridge)

```
plugprobe (single JUCE/C++ CLI, static-link DSP, dynamic syslibs only)
  ├─ audio engine: offline render (processBlock) + live session
  │               (file loop → plugin → out.wav + monitor tap, RT thread)
  ├─ ui engine: open editor in real OS window, snapshot tree, screenshot
  ├─ os-input driver: CGEvent / SendInput / XTest click·drag·scroll·key
  │               with focus-bring-to-front + node→screen mapping + log
  └─ compare kit: null/LUFS/spectral/SDR/cost metrics (ports rx-ab-study)
```

Why: CLI is testable over shell. Agents use CLI JSON directly.

Language: `plugprobe` stays C++/JUCE — VST3/AU/LV2 hosting + foreign editor
embedding are solved there; Rust (`vst3-sys`/`vst3-host`) would reimplement
that layer on immature crates. Rust is reserved for satellites (compare kit,
OS driver) if one outgrows C++; no language split for v0.

## 3. CLI contract

All commands: `plugprobe <cmd> --json <args.json>` → `{ok, data|error}` on
stdout, exit code 0/1. No sleeps/polling; readiness via process exit +
output file existence.

```
scan        --paths <dirs>            → [{id,file,format,params:n}]
inspect     --plugin <id|path>        → {params:[{index,name,label,value,min,max,default,automatable}], editor:{width,height,hasUI}}
snapshot    --plugin <id> [--shot png]→ {nodes:[{id,role,name,bounds,screenXY,value}], screenshot?}
act         --plugin <id> --via os|juce --action {click|drag|scroll|type|key}
            --target <nodeId|x,y> [--to x,y|--text s|--dx dy]
            → {ok, via, paramDelta:[{index,before,after}], uiOnly?, shotAfter?}
            // default --via os: real OS event through the driver (any plugin).
            // --via juce only for JUCE plugins (in-process injection, no permission).
            // every action timestamp-logged; unmoved params → `uiOnly:true`
session     start --plugin <id> --loop in.wav --out take.wav [--monitor on]
            [--sr 48000] [--ui_script u.json]
            → {session, outFile, startedAt, latencyMs}
session     act --session <s> <act args...> [--at_ms t]
            → {ok, atMs, paramDelta, meters:{peakDb,rmsDb}, spectrumHash?}
session     stop --session <s>        → {outFile, durS, eventLog, hash}
            // live path: input loops in real time, agent acts mid-stream,
            // output recorded + monitor tap audible; eventLog aligns each
            // action to a sample position for before/after compare slices
meters      --session <s> [--window_ms 1000]
            → {peakDb, rmsDb, lufsM, crestDb, spectrum:[...bins]}
            // agent's "ears": textual feedback on what a tweak just did
render      --plugin <id> --in a.wav [b.wav] --out out.wav
            --sr 48000 --block 512 [--params_json p.json] [--ui_script u.json]
            [--tail_ms 500] [--bypass false] [--timeout_s 60]
            → {out, sr, nFrames, peakDb, hash}
compare     --a a.wav --b b.wav [--slices events.json] [--cost speech|music]
            → {nullDb, lufsDiff, spectralDist, sdrDb, artifactDb, costDelta}
            // rx-ab-study ports: null test + level-normalized spectral/SDR +
            // artifact residual + task cost (speech intelligibility proxy);
            // --slices scores per eventLog segment (before vs after each act)
```

`--ui_script`: ordered list of `act` steps with optional `at_ms` offsets,
executed via the OS driver before/during render or live session (e.g. click
Learn, wait, click again to freeze). Recorded by `snapshot`, never
hardcoded. Replaces rx-ab-study flag-file handshakes and fixed coords.

## 4. Agent access (CLI only)

No MCP bridge: agents call `plugprobe <cmd> --json <args.json>` directly
over shell. Input schema = CLI args JSON; output = CLI stdout JSON.

### Token budget (hard constraint — every tool call costs the agent money)

- No waiting tools: there is deliberately NO `sleep`/`wait`/poll loop. Bounded
  ops (`render`, `compare`, `act`) block once and return once. Unbounded live
  sessions are agent-paced (`start` … `act` … `stop`); timing decisions need
  no tool calls in between.
- Deltas, not dumps: `act`/`session act` return only `paramDelta` + compact
  meters, never full state. `inspect` output is cached by the agent; repeat
  calls return `{stateHash}` unless something changed.
- Blobs by reference: screenshots, audio takes, full UI trees are written to
  disk and returned as paths. Inline base64 only via explicit
  `fetch_blob --max_kb` (screenshots downscaled by default).
- Paginate the tree: `snapshot [--role slider] [--limit 50] [--offset 0]`
  returns `{total, nodes:[...]}`; the full dump lives at `treePath`.
- Batch actions: `act` accepts an array (one `ui_script` = one tool call for
  N tweaks); per-action results come back aligned in one response.
- Budget CI: snapshot ≤2 KB, act ≤1 KB, meters ≤0.5 KB typical (excluding
  on-demand blobs); a scripted A/B (scan→render×2→compare) targets ≤10 tool
  calls total.

## 5. UI snapshot schema (minimal DOM analog)

```json
{"id":"n12","role":"slider|button|combo|label","name":"Reduction",
 "bounds":[x,y,w,h],"value":0.5,"enabled":true,"nodeId":"n12"}
```

Selector priority: `nodeId` → `name` → `role+index` → raw `x,y`
(recorded coords flagged `fragile:true`, re-resolved against a fresh
`screenshot` on replay). The OS driver maps node→screen coords after
bringing the editor to front, so agents click nodes, not pixels; raw coords
exist only for custom-painted editors with no tree entry. `snapshot` dumps
reusable `ui_script` — the rx-ab-study hardcoded-coords failure, fixed.

## 6. Determinism / correctness invariants

- Same plugin + input + params + sr/block + ui_script → bit-identical
  output hash. Enforce in CI with golden WAVs.
- Offline render never touches network, GUI thread never touches audio
  thread (message queue only, no locks in processBlock).
- Bypass/null test must cancel to >-60 dBFS when plugin is bypassed.
- Screenshot is diagnostic only; pass/fail keys on `paramDelta` + audio hash.
- Live sessions are NOT bit-deterministic (RT scheduling); the determinism
  invariant covers offline `render` only. Live takes log xruns/dropouts and
  align actions by sample position in `eventLog` instead of hashes.
- Audio thread never allocates/locks/does I/O (workspace RT rule); meter
  taps use lock-free ring buffers; GUI/OS-input runs on the message thread.

### CI (GitHub Actions end-to-end)

- `plugprobe` runs headless in CI: offline `render`/`compare`/`probe` need no
  display; editor-dependent `snapshot`/`act` run under `xvfb` (Linux) or the
  runner's GUI session (macOS/Windows), falling back to `--via juce` + params
  plane where OS grants can't exist — no interactive permission prompts in CI.
- Deterministic: fixed `--sr/--block`, golden WAV hashes with `--tol_db`,
  `matrix.csv` + takes uploaded as workflow artifacts.
- Matrix strategy: `os × format` (VST3/AU/LV2),
  `test-feature`/`probe` suites as the test files, via a `setup-plugprobe`
  GitHub Action so downstream plugin repos get e2e with a one-job snippet.

## 7. A/B workflow (the reason this exists)

```
Offline: scan → inspect A/B → snapshot → maps → render on shared inputs
→ compare → matrix.csv {file, plugin, hash, nullDb, lufsDiff, costDelta}
Live (realism): session start (looped input) → act mid-stream via OS driver
→ meters after each tweak → session stop → compare --slices eventLog →
per-tweak before/after clips + scores. Recorded takes are the artifacts
an agent "listened" to.
```

Param maps live in `presets/*.json`, never in code. Inputs are read-only
fixtures; outputs go to `renders/<date>/`.

### Flow A: "test my change" (dev loop, no human in the middle)

`test-feature --plugin <new.vst3> --ref <old.vst3|none> --suite features/x.json`
→ runs ui_script probes (each touched control via OS driver), renders shared
fixtures offline + one live take, compares, writes
`reports/<date>/feature-x/{metrics.json, takes/}`. The agent composes the
human-readable report from those JSON artifacts — no `report` tool,
composition is the agent's job. Pass/fail keys on: touched params move via UI
(`paramDelta` non-empty), bypass nulls, no crashes/xruns, metrics within
tolerance vs ref. One prompt in, one report out.

### Flow B: "learn the black box" (differential probing, rx-ab-study style)

`probe --plugin <blackbox> --inputs fixtures/ --sweeps params|ui` → agent
sweeps each automatable param end-to-end (params plane, cheap) plus
representative UI-only controls (Learn-type buttons via OS driver), renders
each step, `compare --slices` builds a response map
`{param, setting → {nullDb, lufsDiff, spectralDist, costDelta}}`. Output doubles
as prior knowledge for the next modification: which control does what, where it
saturates, what the reference box does that ours doesn't. Same matrix.csv shape
as §7, so A/B and probing share tooling.

Both flows run unattended: bounded tool calls, no mid-run prompts, all context
(meters, eventLog, metrics) machine-readable.

## 8. Platform notes

OS driver: one backend trait, three native backends, zero runtime deps
(PyAutoGUI is behavioral reference + CI oracle only, per static-linking
policy). No OS-isms leak into the core: coordinates are always in
editor-client space; each backend maps to screen space and owns focus.
- macOS (first backend): CGEventPost + full-role AX tree (extends rx-ab-study
  `host/click.swift`/`host/axdump.swift` beyond buttons-only + drag/scroll,
  focus via `AXRaise`); needs Accessibility + Screen Recording grants, fail
  loudly without them. Universal binary.
- Windows: SendInput + UI Automation tree; focus via `SetForegroundWindow`;
  no special grants for own-user editor windows.
- Linux: XTest (X11, `xvfb` in CI) + AT-SPI tree; Wayland via RemoteDesktop
  portal input + screenshot portal (M4, protocol-gated).
Editor always opens in a real, frontmost window at a host-chosen position —
no hardcoded coords anywhere. Static libstdc++/MT, syslibs dynamic per
workspace policy. CI runs snapshot/act/render per OS once its backend lands.
No REAPER.

## 9. Where to start (3 milestones)

1. **M0 spike (1–2 days):** JUCE CLI `scan|inspect|render` for one VST3 +
   golden null-test. Proves the 95% path.
2. **M1 OS driver (native, no new deps):** `snapshot|act|screenshot` via
   hand-rolled OS events on a NON-JUCE plugin (e.g. a proprietary denoiser):
   focus window, full-role AX/UIA dump, click Learn by node id, assert
   `uiOnly` + recorded effect. PyAutoGUI consulted as API reference and
   differential test oracle only. Kills the rx-ab-study fragility (coords,
   flag files, per-version scales).
3. **M2 live session + ears:** `session start|act|stop` + `meters` taps;
   agent tweaks mid-loop and reads peak/LUFS/spectrum deltas.
4. **M3 compare kit:** `compare --slices` ported from
   `rx-ab-study/{compare,delta,metrics,speech_cost}.py` + matrix script
   from `engines.py:render_ours()` minus REAPER/CG hacks + `setup-plugprobe`
   GitHub Action with `os × format` matrix example.

## 10. Open questions

- CLAP support? (needs non-JUCE SDK, defer.)
- Vision grounding for custom-painted editors with no AX/UIA tree?
  (defer to M4, screenshot + driver coords already unblock it.)
- Live-session monitor routing (null sink vs real device mirror)?
  Default file-only + optional device mirror.
- Windows/Linux backends: designed in (trait), built after M1 proves macOS.
- License: DECIDED — own code MIT (see `LICENSE`). Caveat: binaries linking
  JUCE / VST3 SDK distribute under those GPL terms (MIT is GPL-compatible, so
  source stays MIT but the built `plugprobe` binary inherits GPL obligations —
  same situation as Pedalboard/DAWdreamer shipping GPL). A strict MIT
  end-to-end binary would force dropping JUCE/VST3 for AU+CLAP+LV2 only.
