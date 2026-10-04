// GPL-3.0-or-later — Copyright (c) 2026 Luciano Dato — plugprobe
// plugprobe_os_win.cpp: Windows OS backend (STUB — every function fails).
// Real implementation needs OS APIs only (no new third-party deps):
//   tree: UI Automation — IUIAutomation::ElementFromHandle on the hosted
//     HWND, TreeWalker + Invoke/Value/RangeValue/Toggle patterns for
//     press + post-press state re-read (Uiautomationcore, OS-provided).
//   input: SendInput (MOUSEINPUT/KEYBDINPUT) after SetForegroundWindow;
//     node->screen mapping via GetWindowRect (client space, like macOS).
//   capture: PrintWindow with PW_RENDERFULLCONTENT, blank-grab rule as macOS.
//   grants: none for own-user editor windows (UIPI integrity levels apply).
// Until then callers fail loud with UNIMPLEMENTED_M1, never silent no-ops.
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
