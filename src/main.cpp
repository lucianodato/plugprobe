// GPL-3.0-or-later — Copyright (c) 2026 Luciano Dato — plugprobe
// main.cpp: arg parsing, command dispatch, session stubs.
#include "cmds.h"
#include "core.h"

#include <cstring>

using namespace pp;

int main(int argc, char** argv) {
  juce::ScopedJuceInitialiser_GUI juce;
  quietStdout();  // quarantine before any backend can print
  if (argc < 2) {
    emitErr(errObj("ARGS", "usage: plugprobe <cmd> --json <args.json>"));
    return 1;
  }
  juce::String cmd(argv[1]);
  // `plugprobe session start` -> session-start
  if (argc > 2 && juce::String(argv[2]).startsWithChar('-') == false &&
      (cmd == "session")) {
    cmd = "session-" + juce::String(argv[2]);
    std::memmove(&argv[2], &argv[3], sizeof(char*) * (size_t)(argc - 3));
    --argc;
  }
  juce::String jsonPath;
  for (int i = 2; i + 1 < argc; ++i)
    if (juce::String(argv[i]) == "--json") jsonPath = argv[i + 1];
  juce::var args;
  if (jsonPath.isNotEmpty()) {
    juce::File f(jsonPath);
    juce::var parsed;
    if (!juce::JSON::parse(f.loadFileAsString(), parsed) || !parsed.isObject()) {
      emitErr(errObj("JSON", "cannot parse --json file"));
      return 1;
    }
    args = parsed;
  } else {
    args = juce::var(new juce::DynamicObject());
  }
  if (cmd == "scan") return runScan(args);
  if (cmd == "inspect") return runInspect(args);
  if (cmd == "render") return runRender(args);
  if (cmd == "compare") return runCompare(args);
  if (cmd == "snapshot") return runSnapshot(args);
  if (cmd == "act") return runAct(args);
  if (cmd == "session-start") return runSessionStart(args);
  if (cmd == "session-act") return runSessionAct(args);
  if (cmd == "session-stop") return runSessionStop(args);
  if (cmd == "meters") return runMeters(args);

  emitErr(errObj("CMD", "unknown command: " + cmd));
  return 1;
}
