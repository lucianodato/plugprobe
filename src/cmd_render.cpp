// GPL-3.0-or-later — Copyright (c) 2026 Luciano Dato — plugprobe
// cmd_render.cpp: `render` command. See cmds.h.
#include "capture.h"
#include "cmds.h"
#include "gui.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace pp {
int runRender(const juce::var& args)
{
    juce::String inP = jstr(args, "in"), outP = jstr(args, "out");
    if (args.hasProperty("a")) inP = args["a"].toString();
    if (inP.isEmpty() && args["inputs"].isArray() && args["inputs"].size() > 0)
      inP = args["inputs"][0].toString();
    if (inP.isEmpty() || outP.isEmpty()) {
      emitErr(errObj("ARGS", "render needs {plugin,in,out}"));
      return 1;
    }
    double sr = jnum(args, "sr", kDefSr);
    int block = (int)jnum(args, "block", kDefBlock);
    bool bypass = (bool)args["bypass"];
    std::vector<std::vector<float>> ch;
    double fileSr = sr;
    if (!readWav(inP, ch, fileSr)) {
      emitErr(errObj("IO", "cannot read input wav: " + inP));
      return 1;
    }
    std::unique_ptr<juce::AudioPluginInstance> owned, guiOwned;
    juce::AudioPluginInstance* inst = nullptr;
    juce::AudioProcessorEditor* visEd = nullptr;
    juce::String visWhy, edWhy;
    std::unique_ptr<pp::VideoRec> vidRec;  // frame-grab recording; muxed after writeWav
    juce::String vidWhy, vidSaved;
    juce::PluginDescription renderDesc;
    bool wantShot = jstr(args, "shot").isNotEmpty();
    bool wantVis = (bool)args["visible"];
    juce::String videoP = jstr(args, "video");
    // Frame-grab video works headless (no grant, no visible window).
    if (videoP.isNotEmpty() && !plugprobeOsCaps().record) {
      // Diagnostic-only like shots: the take still succeeds with videoSkipped.
      vidWhy = "NO_OS_DRIVER (no screen recorder on this OS); take unaffected";
    }
    // Preset files (presets/*.json) load before plugin lookup so a missing
    // or malformed preset is ARGS anywhere — never a silent dry render.
    juce::var presetMap;
    if (args.hasProperty("params_json")) {
      juce::File pf(args["params_json"].toString());
      if (!pf.existsAsFile()) {
        emitErr(errObj("ARGS", "render: preset file not found: " +
                                   pf.getFullPathName()));
        return 1;
      }
      juce::var presetFile;
      if (!juce::JSON::parse(pf.loadFileAsString(), presetFile)) {
        emitErr(errObj("ARGS", "render: preset file is not JSON: " +
                                   pf.getFullPathName()));
        return 1;
      }
      // Copy out (never self-assign through a member reference: the old
      // target's storage dies mid-assignment -> SIGSEGV in var::operator=,
      // which has no self-assignment guard).
      presetMap = presetFile.hasProperty("params") ? juce::var(presetFile["params"])
                                                   : juce::var(presetFile);
      // NB arrays are objects too in JUCE (isObject && isArray): a preset
      // must be a pure map, so exclude arrays explicitly.
      if (!presetMap.isObject() || presetMap.isArray()) {
        emitErr(errObj("ARGS", "render: preset needs a {param:value} map"));
        return 1;
      }
    }
    // MIDI input (instruments): inline notes and/or a .mid file, parsed and
    // validated before plugin lookup so bad events are ARGS anywhere.
    // Bypass parses but ignores (no instance to feed). Inline notes past the
    // render length are ARGS (explicit intent, likely a bug); file events
    // past the end are dropped (bulk data).
    struct MidiEv {
      int frame = 0;
      juce::MidiMessage msg;
    };
    std::vector<MidiEv> midiEvs;
    {
      int tailN0 = (int)(jnum(args, "tail_ms", 500.0) / 1000.0 * fileSr);
      int totalFr = (int)ch[0].size() + tailN0;
      auto pushAt = [&](double atMs, juce::MidiMessage msg) {
        midiEvs.push_back({(int)(atMs / 1000.0 * fileSr), msg});
      };
      if (auto* ma = args["midi"].getArray()) {
        for (auto& e : *ma) {
          double atMs = (double)e["atMs"];
          int note = (int)e["note"], vel = e.hasProperty("vel") ? (int)e["vel"] : 100;
          double durMs = e.hasProperty("durMs") ? (double)e["durMs"] : 500.0;
          int chn = e.hasProperty("ch") ? (int)e["ch"] : 1;
          if (atMs < 0 || note < 0 || note > 127 || vel < 1 || vel > 127 ||
              durMs <= 0 || chn < 1 || chn > 16) {
            emitErr(errObj("ARGS", "render: midi needs {atMs>=0, note:0..127, "
                                       "vel:1..127, durMs>0, ch:1..16}"));
            return 1;
          }
          int onFr = (int)(atMs / 1000.0 * fileSr);
          if (onFr >= totalFr) {
            emitErr(errObj("ARGS", "render: midi note at " +
                                       juce::String(atMs, 1) +
                                       "ms is past the render length"));
            return 1;
          }
          pushAt(atMs, juce::MidiMessage::noteOn(chn, note, (juce::uint8)vel));
          double offMs = atMs + durMs;
          if ((int)(offMs / 1000.0 * fileSr) <= onFr) offMs = atMs + 1;
          pushAt(offMs, juce::MidiMessage::noteOff(chn, note));
        }
      } else if (args.hasProperty("midi")) {
        emitErr(errObj("ARGS", "render: midi needs an array of "
                                   "{atMs,note,vel?,durMs?,ch?}"));
        return 1;
      }
      juce::String mfP = jstr(args, "midi_file");
      if (mfP.isNotEmpty()) {
        juce::File mf(mfP);
        if (!mf.existsAsFile()) {
          emitErr(errObj("ARGS", "render: midi file not found: " +
                                     mf.getFullPathName()));
          return 1;
        }
        juce::FileInputStream fis(mf);
        juce::MidiFile mid;
        if (!mid.readFrom(fis)) {
          emitErr(errObj("ARGS", "render: not a MIDI file: " +
                                     mf.getFullPathName()));
          return 1;
        }
        int tf = mid.getTimeFormat();
        if (tf <= 0) {
          emitErr(errObj("ARGS", "render: SMPTE-timed MIDI unsupported"));
          return 1;
        }
        // Tempo map (default 120bpm) then tick->second walk per event.
        std::vector<std::pair<int, double>> tempo{{0, 0.5}};
        std::vector<std::pair<int, juce::MidiMessage>> ticked;
        for (int t = 0; t < mid.getNumTracks(); ++t) {
          auto* sq = mid.getTrack(t);
          for (int i = 0; i < sq->getNumEvents(); ++i) {
            auto& me = sq->getEventPointer(i)->message;
            int tick = (int)me.getTimeStamp();
            if (me.isTempoMetaEvent() && tick >= 0)
              tempo.push_back({tick, me.getTempoSecondsPerQuarterNote()});
            else if (tick >= 0)
              ticked.push_back({tick, me});
          }
        }
        std::sort(tempo.begin(), tempo.end());
        std::sort(ticked.begin(), ticked.end(),
                  [](auto& a, auto& b) { return a.first < b.first; });
        size_t ti = 0;
        double sec = 0;
        int lastTick = 0;
        for (auto& [tick, me] : ticked) {
          while (ti + 1 < tempo.size() && tempo[ti + 1].first <= tick) {
            ++ti;
            sec += (tempo[ti].first - lastTick) / (double)tf * tempo[ti - 1].second;
            lastTick = tempo[ti].first;
          }
          double atSec = sec + (tick - lastTick) / (double)tf * tempo[ti].second;
          int fr = (int)(atSec * fileSr);
          if (fr >= 0 && fr < totalFr) midiEvs.push_back({fr, me});
        }
      }
      std::sort(midiEvs.begin(), midiEvs.end(),
                [](auto& a, auto& b) { return a.frame < b.frame; });
    }
    // Shared drain index: the main pass then the tail flush consume frames
    // in order, so one cursor covers both (note-offs land in the tail).
    size_t midiIdx = 0;
    auto fillMidi = [&](juce::MidiBuffer& mb, size_t baseFr, int m) {
      mb.clear();
      while (midiIdx < midiEvs.size() &&
             midiEvs[midiIdx].frame < (int)(baseFr + (size_t)m)) {
        int off = midiEvs[midiIdx].frame - (int)baseFr;
        if (off >= 0) mb.addEvent(midiEvs[midiIdx].msg, off);
        ++midiIdx;
      }
    };
    if (!bypass) {
      juce::PluginDescription d;
      if (!findPlugin(jstr(args, "plugin"), argPaths(args), d)) {
        emitErr(errObj("NOT_FOUND", "plugin not found"));
        return 1;
      }
      renderDesc = d;
      // GUI-backed processing instance when the UI will be shown/captured or
      // clicked, so the window reflects the audio being rendered (live
      // meters, real clicks). The default path stays headless (byte-identical).
      // Timeline entries are pre-scanned: the full parse happens later.
      bool wantGui = wantShot || wantVis;
      wantGui = wantGui || (videoP.isNotEmpty() && plugprobeOsCaps().record);
      if (!wantGui) {
        if (auto* tl0 = args["timeline"].getArray()) {
          for (auto& e0 : *tl0) {
            bool coordClick = e0.hasProperty("click") &&
                              e0["click"].isObject() &&
                              e0["click"].hasProperty("x");
            if (jstr(e0, "click").isNotEmpty() || coordClick ||
                jstr(e0, "shot").isNotEmpty() || e0.hasProperty("slider")) {
              wantGui = true;
              break;
            }
          }
        }
      }
      if (wantGui && guiCapable(d)) {
        juce::String ge;
        guiOwned = guiCreate(d, sr, block, ge);
        if (guiOwned != nullptr)
          inst = guiOwned.get();
        else if (wantVis)
          visWhy = juce::String("gui-instantiate: ") + ge.substring(0, 120);
      } else if (wantVis) {
        visWhy = "unsupported-format";
      }
      if (inst == nullptr) {
        juce::String e;
        owned = instantiate(d, sr, block, e);
        if (owned == nullptr) {
          emitErr(errObj("INSTANTIATE", e));
          return 1;
        }
        inst = owned.get();
      }
      // Fail loud on unknown targets: silent passthrough renders are worse
      // than errors (every param-plane entry routes through here).
      // Preset first, explicit params second (explicit wins on conflict).
      if (presetMap.isObject()) {
        if (juce::String u = unknownTargetIn(*inst, presetMap);
            u.isNotEmpty()) {
          emitErr(errObj("ARGS", "render: unknown param '" + u +
                                     "' (have: " + validParamNames(*inst) +
                                     ")"));
          return 1;
        }
        applyParams(*inst, presetMap);
      }
      if (args.hasProperty("params")) {
        if (juce::String u = unknownTargetIn(*inst, args["params"]);
            u.isNotEmpty()) {
          emitErr(errObj("ARGS", "render: unknown param '" + u + "' (have: " +
                                     validParamNames(*inst) + ")"));
          return 1;
        }
        applyParams(*inst, args["params"]);
      }
      // ui_script: juce-plane steps only (param targets); os-only skipped.
      if (auto* sc = args["ui_script"].getArray()) {
        for (auto& s : *sc) {
          juce::String tgt = s.hasProperty("target") ? s["target"].toString() : "";
          if (tgt.isEmpty()) continue;
          if (findParamIndex(*inst, tgt) < 0) {
            emitErr(errObj("ARGS", "render: unknown ui_script target '" +
                                       tgt + "' (have: " +
                                       validParamNames(*inst) + ")"));
            return 1;
          }
          auto* m = new juce::DynamicObject();
          m->setProperty(tgt, s.hasProperty("value") ? (float)s["value"] : 1.f);
          applyParams(*inst, juce::var(m));
        }
      }
      inst->setRateAndBufferSizeDetails(sr, block);
      inst->prepareToPlay(sr, block);
    }
    int nCh = std::max(1, (int)ch.size());
    size_t n = ch[0].size();
    std::vector<std::vector<float>> out(nCh, std::vector<float>(n, 0));
    juce::String shotSaved, shotWhy;
    juce::Array<juce::var> shotsArr, clicksArr, slidersArr;
    bool hasEntryShot = false, hasEntryClick = false, hasEntrySlider = false;
    if (bypass) {
      out = ch;
    } else {
      // timeline [{atMs, params, shot?, click?}] on the same instance.
      // Per-entry `shot` captures the live editor right after the switch
      // lands; per-entry `click` presses a snapshot node id — or raw
      // screen {x,y} for AX-empty custom-painted editors — on the live
      // window (native buttons no param can reach). One render call is the
      // scripted open → click → audio → click → capture session equivalent.
      struct Ev {
        size_t frame;
        double atMs;
        juce::var map;
        juce::String shot;
        juce::String click;
        bool clickIsCoord = false;
        double clickX = 0, clickY = 0;
        juce::String sliderTarget;  // AX slider set (grant-free, headless)
        double sliderValue = 0;
        bool hasSlider = false;
      };
      std::vector<Ev> evs;
      if (auto* tl = args["timeline"].getArray()) {
        int ti = 0;
        for (auto& e : *tl) {
          double atMs = e.hasProperty("atMs")
                            ? (double)e["atMs"]
                            : (double)e["at_ms"];
          juce::var map = e.hasProperty("params") ? e["params"] : e["map"];
          bool clickIsCoord = e.hasProperty("click") && e["click"].isObject() &&
                              e["click"].hasProperty("x") &&
                              e["click"].hasProperty("y");
          if (e.hasProperty("click") && e["click"].isObject() &&
              !clickIsCoord) {
            emitErr(errObj("ARGS", "render: timeline click object needs {x,y} "
                                   "screen coords (e.g. {click:{x:900,y:600}})"));
            return 1;
          }
          double clickX = clickIsCoord ? (double)e["click"]["x"] : 0;
          double clickY = clickIsCoord ? (double)e["click"]["y"] : 0;
          // Slider set: widget-level control for param-less sliders
          // (grant-free AX value, headless-safe — unlike HID drag).
          bool hasSlider = e.hasProperty("slider") && e["slider"].isObject();
          juce::String sliderTarget;
          double sliderValue = 0;
          if (e.hasProperty("slider") && !hasSlider) {
            emitErr(errObj("ARGS", "render: timeline slider needs "
                                       "{target,value} (value in native units, "
                                       "snapshot shows current)"));
            return 1;
          }
          if (hasSlider) {
            sliderTarget = jstr(e["slider"], "target");
            auto sv = e["slider"]["value"];
            if (sliderTarget.isEmpty() || (!sv.isDouble() && !sv.isInt())) {
              emitErr(errObj("ARGS", "render: timeline slider needs "
                                         "{target,value} (value in native "
                                         "units, snapshot shows current)"));
              return 1;
            }
            sliderValue = (double)sv;
            hasEntrySlider = true;
          }
          if (!map.isObject()) {
            if (jstr(e, "click").isEmpty() && !clickIsCoord &&
                jstr(e, "shot").isEmpty() && !hasSlider) {
              emitErr(errObj("ARGS", "render: timeline entry needs "
                                         "{atMs,params} (or click/shot/slider)"));
              return 1;
            }
            map = juce::var(new juce::DynamicObject());  // click/shot/slider only
          }
          if (juce::String u = unknownTargetIn(*inst, map); u.isNotEmpty()) {
            emitErr(errObj("ARGS", "render: timeline[" +
                                       juce::String(ti) + "] unknown param '" +
                                       u + "' (have: " +
                                       validParamNames(*inst) + ")"));
            return 1;
          }
          size_t f = (size_t)std::max(0.0, atMs / 1000.0 * sr);
          juce::String eshot = jstr(e, "shot");
          juce::String eclick = clickIsCoord ? juce::String("") : jstr(e, "click");
          if (eshot.isNotEmpty()) hasEntryShot = true;
          if (eclick.isNotEmpty() || clickIsCoord) hasEntryClick = true;
          const auto caps = plugprobeOsCaps();
          if ((eclick.isNotEmpty() || clickIsCoord) &&
              !(caps.editor && caps.tree && caps.input)) {
            emitErr(errObj("NO_OS_DRIVER", "render: timeline clicks need the "
                                               "OS UI driver, unavailable on "
                                               "this OS"));
            return 1;
          }
          if (hasSlider && !(caps.editor && caps.tree)) {
            emitErr(errObj("NO_OS_DRIVER", "render: timeline sliders need the "
                                               "OS UI driver, unavailable on "
                                               "this OS"));
            return 1;
          }
          evs.push_back({f, atMs, map, eshot, eclick, clickIsCoord, clickX,
                         clickY, sliderTarget, sliderValue, hasSlider});
          ++ti;
        }
        std::sort(evs.begin(), evs.end(),
                  [](const Ev& a, const Ev& b) { return a.frame < b.frame; });
      }
      // Editor for mid-render shots/clicks/sliders/video: hidden unless
      // visible. Only opened when captures or clicks were requested, so
      // pure-headless renders stay byte-identical.
      if (guiOwned != nullptr && visEd == nullptr &&
          (wantVis || wantShot || hasEntryShot || hasEntryClick ||
           hasEntrySlider || videoP.isNotEmpty())) {
        juce::String why;
        visEd = openEditor(*guiOwned, wantVis, why);
        if (visEd == nullptr) {
          edWhy = why;
          if (wantShot) shotWhy = why;
          if (wantVis) visWhy = why;
        } else if (wantVis) {
          pumpMessages();  // let the fresh window paint before audio starts
        }
      } else if ((hasEntryClick || hasEntrySlider) && guiOwned == nullptr) {
        edWhy =
            "no GUI-backed instance (unsupported format or instantiate failed)";
      }
      if (hasEntryClick && visEd == nullptr) {
        emitErr(errObj("ARGS", "render: timeline clicks need a live editor "
                               "window: " + edWhy));
        return 1;
      }
      if (hasEntrySlider && visEd == nullptr) {
        emitErr(errObj("ARGS", "render: timeline sliders need a live editor "
                               "window: " + edWhy));
        return 1;
      }
      // Opt-in screen recording: captures the live window for the whole
      // paced pass; the take WAV is muxed in as audio when it lands.
      // Diagnostic-only (like shots): a dead recorder never fails the take.
      if (videoP.isNotEmpty() && plugprobeOsCaps().record) {
        if (visEd == nullptr) {
          vidWhy = edWhy.isNotEmpty() ? edWhy : "no-editor";
        } else {
          vidRec = pp::videoStart(visEd->getWindowHandle());
        }
      }
      size_t ei = 0, bc = 0;
      juce::AudioBuffer<float> blk(std::max(2, nCh), block);
      juce::MidiBuffer midi;
      // Wall-clock anchor for 1x pacing while the live window is open.
      double paceT0 = juce::Time::getMillisecondCounterHiRes();
      for (size_t pos = 0; pos < n; pos += (size_t)block) {
        bool moved = false;
        while (ei < evs.size() && evs[ei].frame <= pos) {
          applyParams(*inst, evs[ei].map);
          moved = true;
          if (evs[ei].click.isNotEmpty() || evs[ei].clickIsCoord) {
            // Native click on the live window (buttons no param can reach).
            auto* k = new juce::DynamicObject();
            k->setProperty("atMs", evs[ei].atMs);
            if (evs[ei].clickIsCoord) {
              // HID click: needs the on-screen window + Accessibility grant.
              if (!wantVis) {
                emitErr(errObj("ARGS", "render: timeline coordinate clicks need "
                                       "\"visible\":true (HID events cannot "
                                       "reach the offscreen window; node-id "
                                       "clicks work headless via AX)"));
                return 1;
              }
              if (!plugprobeInputGranted()) {
                emitErr(errObj("AX_UNTRUSTED",
                               "render: timeline coordinate clicks need the "
                               "Accessibility grant for THIS plugprobe binary"));
                return 1;
              }
              plugprobeFocusWindow(visEd->getWindowHandle());  // else click only activates
              if (!plugprobeAxClickAt(evs[ei].clickX, evs[ei].clickY)) {
                emitErr(errObj("AX_CLICK",
                               "render: timeline coordinate click failed"));
                return 1;
              }
              auto* kc = new juce::DynamicObject();
              kc->setProperty("x", evs[ei].clickX);
              kc->setProperty("y", evs[ei].clickY);
              kc->setProperty("pressed", true);
              kc->setProperty("fragile", true);  // raw coords: re-ground on replay
              if (juce::var hit =
                      hitNodeAt(visEd->getWindowHandle(), evs[ei].clickX,
                                evs[ei].clickY);
                  hit.isObject())
                kc->setProperty("hit", hit);
              k->setProperty("click", juce::var(kc));
            } else {
            double cx = 0, cy = 0;
            int rc = plugprobeAxPressById(evs[ei].click.toRawUTF8(),
                                      visEd->getWindowHandle(), &cx, &cy);
            if (rc <= 0) {
              emitErr(errObj("ARGS", juce::String("render: timeline click ") +
                                         (rc == 0 ? "unknown node '"
                                                  : "press failed on '") +
                                         evs[ei].click +
                                         "' (snapshot lists ids)"));
              return 1;
            }
            auto* kc = new juce::DynamicObject();
            kc->setProperty("node", evs[ei].click);
            kc->setProperty("pressed", true);
            auto* kp = new juce::DynamicObject();
            kp->setProperty("x", cx);
            kp->setProperty("y", cy);
            kc->setProperty("center", juce::var(kp));
            for (auto& nn : plugprobeAxDump(visEd->getWindowHandle())) {
              if (juce::String(nn.id) == evs[ei].click) {
                if (!nn.value.empty())
                  kc->setProperty("state", juce::String(nn.value));
                break;
              }
            }
            k->setProperty("click", juce::var(kc));
            }
            clicksArr.add(juce::var(k));
            pumpMessages();  // let the press dispatch before audio resumes
          }
          if (evs[ei].hasSlider) {
            // Grant-free widget set on the live window (no HID): reaches
            // sliders no param can touch. Views attach async, so wait for
            // the node instead of racing it.
            auto* ks = new juce::DynamicObject();
            ks->setProperty("atMs", evs[ei].atMs);
            double actual = 0;
            juce::String serr;
            if (!axWaitForId(visEd->getWindowHandle(), evs[ei].sliderTarget,
                             10000)) {
              serr = "unknown node '" + evs[ei].sliderTarget +
                     "' (snapshot lists ids)";
            } else {
              int src = plugprobeAxSetValueById(
                  evs[ei].sliderTarget.toRawUTF8(), visEd->getWindowHandle(),
                  evs[ei].sliderValue, &actual);
              if (src < 0) serr = "set failed on '" + evs[ei].sliderTarget + "'";
            }
            if (serr.isNotEmpty()) {
              emitErr(errObj("ARGS", juce::String("render: timeline slider ") +
                                         serr));
              return 1;
            }
            auto* ksc = new juce::DynamicObject();
            ksc->setProperty("node", evs[ei].sliderTarget);
            ksc->setProperty("set", evs[ei].sliderValue);
            ksc->setProperty("state", actual);
            ks->setProperty("slider", juce::var(ksc));
            slidersArr.add(juce::var(ks));
            pumpMessages();  // let the set dispatch before audio resumes
          }
          if (evs[ei].shot.isNotEmpty()) {
            auto* s = new juce::DynamicObject();
            s->setProperty("atMs", evs[ei].atMs);
            if (visEd != nullptr) {
              if (!plugprobeOsCaps().editor) {
                s->setProperty("shotSkipped", "NO_OS_DRIVER");
              } else {
                juce::String saved, why;
                if (noteCapture(plugprobeSaveNSViewShot(visEd->getWindowHandle(),
                                                    evs[ei].shot.toRawUTF8(),
                                                    nullptr, nullptr),
                                evs[ei].shot, saved, why))
                  s->setProperty("screenshot", saved);
                else
                  s->setProperty("shotSkipped", why);
              }
            } else {
              s->setProperty("shotSkipped",
                             visWhy.isNotEmpty() ? visWhy : "no-editor");
            }
            shotsArr.add(juce::var(s));
          }
          ++ei;
        }
        if (moved) pumpMessages();  // flush plugin-side deferred updates
        if (visEd != nullptr && (bc++ % 4) == 0) pumpMessages();  // live UI
        int m = (int)std::min((size_t)block, n - pos);
        blk.clear();
        for (int c = 0; c < blk.getNumChannels(); ++c) {
          float* w = blk.getWritePointer(c);
          const auto& src = ch[(size_t)c % ch.size()];
          for (int i = 0; i < m; ++i) w[i] = src[pos + (size_t)i];
        }
        fillMidi(midi, pos, m);
        inst->processBlock(blk, midi);
        for (int c = 0; c < nCh; ++c) {
          const float* r = blk.getReadPointer(c % blk.getNumChannels());
          for (int i = 0; i < m; ++i) out[(size_t)c][pos + (size_t)i] = r[i];
        }
        if (visEd != nullptr) paceToRealtime(paceT0, pos + (size_t)m, sr);
        if (vidRec != nullptr) {
          pumpMessages();  // fresh paint before the grab
          pp::videoGrab(*vidRec);
        }
      }
      int tailN = (int)(jnum(args, "tail_ms", 500.0) / 1000.0 * sr);
      if (tailN > 0) {
        size_t base = n;
        for (auto& c : out) c.resize(n + (size_t)tailN, 0);
        juce::AudioBuffer<float> blk(std::max(2, nCh), block);
        juce::MidiBuffer midi;
        for (int pos = 0; pos < tailN; pos += block) {
          int m = std::min(block, tailN - pos);
          blk.clear();
          fillMidi(midi, base + (size_t)pos, m);
          inst->processBlock(blk, midi);
          for (int c = 0; c < nCh; ++c) {
            const float* r = blk.getReadPointer(c % blk.getNumChannels());
            for (int i = 0; i < m; ++i)
              out[(size_t)c][base + (size_t)pos + (size_t)i] = r[i];
          }
          if (visEd != nullptr)
            paceToRealtime(paceT0, n + (size_t)(pos + m), sr);
          if (vidRec != nullptr) {
            pumpMessages();
            pp::videoGrab(*vidRec);
          }
        }
      }
      inst->releaseResources();
      juce::String rsp = jstr(args, "shot");
      if (guiOwned != nullptr && (rsp.isNotEmpty() || visEd != nullptr)) {
        // Capture the instance that actually rendered (post-render state).
        if (visEd == nullptr) {
          juce::String why;
          visEd = openEditor(*guiOwned, wantVis, why);
          if (visEd == nullptr) {
            if (rsp.isNotEmpty()) shotWhy = why;
            if (wantVis) visWhy = why;
          }
        }
        if (visEd != nullptr && rsp.isNotEmpty()) {
          if (!plugprobeOsCaps().editor) {
            shotWhy = "NO_OS_DRIVER";
          } else {
            noteCapture(plugprobeSaveNSViewShot(visEd->getWindowHandle(),
                                            rsp.toRawUTF8(), nullptr, nullptr),
                        rsp, shotSaved, shotWhy);
          }
        }
      } else if (rsp.isNotEmpty()) {
        juce::String why, visDummy;
        shotSaved =
            saveEditorShot(renderDesc, sr, block, rsp, why, visDummy, false, 0);
        if (shotSaved.isEmpty()) shotWhy = why;
      }
      if (visEd != nullptr) {
        if (wantVis) holdUi((int)jnum(args, "holdMs", 0));
        if (visEd->isOnDesktop()) visEd->removeFromDesktop();
        visEd = nullptr;
      }
    }
    if (!writeWav(outP, out, sr)) {
      emitErr(errObj("IO", "cannot write output wav"));
      return 1;
    }
    if (vidRec != nullptr) {
      juce::String fe;
      juce::File take = juce::File::isAbsolutePath(outP)
                            ? juce::File(outP)
                            : juce::File::getCurrentWorkingDirectory()
                                  .getChildFile(outP);
      if (pp::videoFinish(*vidRec, take, juce::File(videoP), fe))
        vidSaved = videoP;
      else if (vidWhy.isEmpty())
        vidWhy = fe;
      vidRec.reset();
    }
    auto* o = new juce::DynamicObject();
    o->setProperty("out", outP);
    o->setProperty("sr", sr);
    o->setProperty("nFrames", (double)out[0].size());
    o->setProperty("inFrames", (double)n);
    o->setProperty("outFrames", (double)out[0].size());
    o->setProperty("tailFrames", (double)(out[0].size() - n));
    o->setProperty("tailNote", "in-aligned; flush tail appended at end");
    if (hasEntryShot) o->setProperty("shots", juce::var(shotsArr));
    if (hasEntryClick) o->setProperty("clicks", juce::var(clicksArr));
    if (hasEntrySlider) o->setProperty("sliders", juce::var(slidersArr));
    o->setProperty("peakDb", peakDbOf(out));
    o->setProperty("hash", hashHex(out));
    if (!midiEvs.empty()) o->setProperty("midiEvents", (double)midiEvs.size());
    if (!bypass) setBundleProps(o, renderDesc);
    if (jstr(args, "shot").isNotEmpty()) {
      if (shotSaved.isNotEmpty())
        o->setProperty("screenshot", shotSaved);
      else
        o->setProperty("shotSkipped", bypass ? "bypass-no-instance"
                                             : shotWhy.isNotEmpty()
                                                   ? shotWhy
                                                   : "no-editor-or-headless");
    }
    if (videoP.isNotEmpty()) {
      if (vidSaved.isNotEmpty())
        o->setProperty("video", vidSaved);
      else
        o->setProperty("videoSkipped", bypass ? "bypass-no-instance"
                                              : vidWhy.isNotEmpty()
                                                    ? vidWhy
                                                    : "no-editor-or-headless");
    }
    if (wantVis && bypass && visWhy.isEmpty()) visWhy = "bypass-no-instance";
    if (wantVis) {
      if (visWhy.isNotEmpty())
        o->setProperty("visibleSkipped", visWhy);
      else
        o->setProperty("visible", true);
    }
    emitOk(juce::var(o));
    return 0;
}
}  // namespace pp
