// GPL-3.0-or-later — Copyright (c) 2026 Luciano Dato — plugprobe
// main.cpp: arg parsing, command dispatch.
#include "cmds.h"
#include "core.h"

#include <cstdlib>
#include <cstring>
#include <iostream>

using namespace pp;

#ifndef PLUGPROBE_VERSION
#define PLUGPROBE_VERSION "0.0.0-unknown"
#endif

// No-plugin paths: answer directly, never via the crash-isolation child.
static bool isHelp(int argc, char** argv) {
  if (argc < 2) return false;
  juce::String a(argv[1]);
  return a == "--help" || a == "-h" || a == "help";
}
static bool isVersion(int argc, char** argv) {
  if (argc < 2) return false;
  juce::String a(argv[1]);
  return a == "--version" || a == "-v" || a == "version";
}
static void printHelp() {
  std::printf(
      "plugprobe %s — agent-driven CLI for hosting audio plugins\n"
      "usage: plugprobe <cmd> --json <args.json|json-string|->\n"
      "  --json - reads args from stdin (heredoc-friendly)\n"
      "commands:\n"
      "  scan           find plugins (standard paths by default)\n"
      "  inspect        automatable params + editor size\n"
      "  render         offline render (params/timeline/midi/video)\n"
      "  compare        A/B two audio files\n"
      "  snapshot       UI node ids for act targeting\n"
      "  act            detached click/drag/type/slider-set probe\n"
      "  session start|act|stop  agent-paced takes (events replayed at stop)\n"
      "  meters         loudness/spectrum of a session loop or file\n"
      "  manual         locate bundled plugin docs (agent reads them)\n"
      "errors are JSON {ok:false,error:{code,message}}; codes: ARGS NOT_FOUND "
      "NO_OS_DRIVER CRASH AX_UNTRUSTED RENDER IO\n"
      "see README.md / SKILL.md for the agent workflow\n",
      PLUGPROBE_VERSION);
  std::fflush(stdout);
}

// Crash isolation: foreign plugin binaries can segfault/abort at load,
// process, or teardown (exit-time timer races are documented in core.cpp).
// The parent re-runs itself as a child (PLUGPROBE_CHILD=1) and forwards the
// child's stdout verbatim, so the agent always gets JSON: a valid envelope
// passes through untouched (exit code included); anything else (dead pipe,
// partial print) is a fail-loud CRASH, never a silent hang or empty output.
// ponytail: ChildProcess over fork — portable (Windows has no fork), one
// seam for all commands; a hung plugin still hangs (same as in-process).
static int runIsolated(int argc, char** argv) {
#if JUCE_WINDOWS
  _putenv("PLUGPROBE_CHILD=1");
#else
  ::setenv("PLUGPROBE_CHILD", "1", 1);
#endif
  juce::ChildProcess cp;
  juce::StringArray a;
  a.add(juce::File::getSpecialLocation(juce::File::currentExecutableFile)
            .getFullPathName());
  for (int i = 1; i < argc; ++i) a.add(argv[i]);
  if (!cp.start(a)) return -1;  // fail open: caller runs in-process as today
  // Drain while waiting: plugin binaries chatter on stderr (duplicate-class
  // warnings, lilv notes) and a full pipe would deadlock a blind wait.
  // -1 wraps to ~49d inside JUCE (uint32): unbounded, like in-process hangs.
  juce::String all;
  while (cp.isRunning()) {
    all += cp.readAllProcessOutput();
    juce::Thread::sleep(5);
  }
  all += cp.readAllProcessOutput();
  juce::String last;
  for (auto& ln : juce::StringArray::fromLines(all))
    if (ln.trim().isNotEmpty()) last = ln;
  juce::var env;
  if (juce::JSON::parse(last, env) && env.isObject() &&
      env.hasProperty("ok")) {
    std::printf("%s\n", last.toRawUTF8());
    std::fflush(stdout);
    return (int)cp.getExitCode();
  }
  auto* o = new juce::DynamicObject();
  o->setProperty("ok", false);
  auto* e = new juce::DynamicObject();
  e->setProperty("code", "CRASH");
  e->setProperty("message",
                 "plugin host died mid-command (exit " +
                     juce::String((int)cp.getExitCode()) +
                     "); rerun headless/bypass to isolate");
  o->setProperty("error", juce::var(e));
  std::printf("%s\n", juce::JSON::toString(juce::var(o), true).toRawUTF8());
  std::fflush(stdout);
  return 1;
}

int main(int argc, char** argv) {
  juce::ScopedJuceInitialiser_GUI juce;
  if (isHelp(argc, argv)) {
    printHelp();
    return 0;
  }
  if (isVersion(argc, argv)) {
    std::printf("plugprobe %s\n", PLUGPROBE_VERSION);
    return 0;
  }
  if (juce::SystemStats::getEnvironmentVariable("PLUGPROBE_CHILD", "0") ==
      "0") {
    int rc = runIsolated(argc, argv);
    if (rc >= 0) return rc;
  }
  if (std::getenv("PLUGPROBE_INJECT_CRASH") != nullptr) std::abort();
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
    juce::String txt;
    if (jsonPath == "-") {
      // Heredoc/pipe: printf ... | plugprobe render --json -
      while (!std::cin.eof()) {
        char buf[4096];
        std::cin.read(buf, sizeof(buf));
        txt += juce::String(buf, (size_t)std::cin.gcount());
      }
    } else {
      // Bare JSON on the command line saves humans a temp file; a real path
      // still wins when it exists.
      juce::File f(jsonPath);
      txt = f.existsAsFile() ? f.loadFileAsString() : jsonPath;
    }
    juce::var parsed;
    if (!juce::JSON::parse(txt, parsed) || !parsed.isObject()) {
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
  if (cmd == "manual") return runManual(args);

  emitErr(errObj("CMD", "unknown command: " + cmd));
  return 1;
}
