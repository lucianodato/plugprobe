// GPL-3.0-or-later — Copyright (c) 2026 Luciano Dato — plugprobe
// plugprobe_os.h: OS-layer interface. Window fronting, editor capture, UI
// tree dump and synthetic input — everything that touches native APIs.
// One backend file per OS implements these 1:1:
//   macOS: plugprobe_os_mac.mm (real, AX/CGEvent/screencapture)
//   Windows: plugprobe_os_win.cpp (stub: UI Automation + SendInput + PrintWindow)
//   Linux: plugprobe_os_linux.cpp (stub: AT-SPI + XTest + XGetImage)
// Handles are opaque void* (NSView / HWND / X11 Window) so callers stay
// platform-agnostic. Backends that cannot do the job return failure;
// callers translate that to loud errors (NO_OS_DRIVER), never no-ops.
#pragma once

#include <string>
#include <vector>

struct PlugprobeAxNode {
  std::string id, role, name;
  bool enabled = true;
  std::string value;
  double x = 0, y = 0, w = 0, h = 0;  // screen space, top-left origin
};

// Driver features this OS backend provides. Callers check these and fail loud
// (NO_OS_DRIVER) when one is false; they never silently succeed.
struct PlugprobeOsCaps {
  bool editor = false;  // ShowFront + editor shots (NSView / HWND / X11 window)
  bool tree = false;    // AxDump / AxPressById / AxSetValueById
  bool input = false;   // AxClickAt / AxDragAt / AxTypeText
  bool record = false;  // plugprobeGrabFrame (video)
};
PlugprobeOsCaps plugprobeOsCaps();
// Synthetic input permission. macOS needs the Accessibility grant; ad-hoc
// rebuilds change the binary hash and invalidate it, so check at runtime.
bool plugprobeInputGranted();

// --- Capture (opt-in `shot` PNGs; 1 ok / 0 fail / -1 blank) ---
int plugprobeSaveNSViewShot(void* nsView, const char* path, int* w, int* h);
int plugprobeSaveWindowShot(void* nsView, const char* path, int* w, int* h);
bool plugprobeShowFront(void* nsView);

// --- UI tree + input (snapshot/act) ---
// press returns 1 pressed / 0 unknown id / -1 press failed.
std::vector<PlugprobeAxNode> plugprobeAxDump(void* nsView);
int plugprobeAxPressById(const char* nodeId, void* nsView, double* cx,
                         double* cy);
// Slider set without HID: grant-free (own-process AX), headless-safe.
// value is in the control's NATIVE units (snapshot shows the current one).
// Returns 1 set (re-read into actual) / 0 unknown id / -1 fail.
int plugprobeAxSetValueById(const char* nodeId, void* nsView, double value,
                            double* actual);
// Raw screen-coordinate click (mouse space, top-left) for AX-empty
// custom-painted editors: grounded by a visible screenshot, flagged fragile.
bool plugprobeAxClickAt(double x, double y);
void plugprobeFocusWindow(void* nsView);
bool plugprobeAxDragAt(double x, double y, double dx, double dy);
bool plugprobeAxTypeText(const char* text);
void plugprobePumpApp(double seconds);
// One-shot activation attempt for headless sessions (CI): console tools may
// start with no GUI presence, leaving the AX window list empty despite the
// trust grant. Only call when the AX tree comes back empty — interactive
// runs never touch it (no dock bounce, no focus steal).
void plugprobeTryActivate();

// --- Frame grab (shots and video are encoded in capture.cpp, shared) ---
// The editor view renders itself into an RGB bitmap: works hidden, no Screen
// Recording grant, no visible window. false = no pixels this time.
struct PlugprobeRgbFrame {
  int w = 0, h = 0;
  std::vector<unsigned char> rgb;  // top-down, 3 bytes per pixel
};
bool plugprobeGrabFrame(void* hv, PlugprobeRgbFrame& out);
