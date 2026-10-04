// GPL-3.0-or-later — Copyright (c) 2026 Luciano Dato — plugprobe
// plugprobe_os_linux.cpp: Linux OS backend (STUB — every function fails).
// Real implementation needs OS APIs only (no new third-party deps):
//   tree: AT-SPI2 (libatspi registry; our hosted window is in-process,
//     so no sandbox negotiation for the tree walk itself).
//   input: XTest (XTestFakeButtonEvent/KeyEvent) on X11, `xvfb` in CI;
//     node->screen via XTranslateCoordinates. Wayland has no synthetic
//     input: must go through the RemoteDesktop portal (fail loud there).
//   capture: XGetImage/XShmGetImage on X11; Wayland needs the ScreenCast
//     portal (async, session-gated) — same blank-grab rule as macOS.
//   grants: none on X11; portals prompt per-session on Wayland.
// Until then callers fail loud with NO_OS_DRIVER, never silent no-ops.
#include "plugprobe_os.h"

int plugprobeSaveNSViewShot(void*, const char*, int*, int*) { return 0; }
int plugprobeSaveWindowShot(void*, const char*, int*, int*) { return 0; }
bool plugprobeShowFront(void*) { return false; }

std::vector<PlugprobeAxNode> plugprobeAxDump(void*) { return {}; }
int plugprobeAxPressById(const char*, void*, double*, double*) { return -1; }
bool plugprobeAxClickAt(double, double) { return false; }
void plugprobeFocusWindow(void*) {}
bool plugprobeAxDragAt(double, double, double, double) { return false; }
bool plugprobeAxTypeText(const char*) { return false; }
void plugprobePumpApp(double) {}
