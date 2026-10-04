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

// --- Capture (opt-in `shot` PNGs; 1 ok / 0 fail / -1 blank) ---
int plugprobeSaveNSViewShot(void* nsView, const char* path, int* w, int* h);
int plugprobeSaveWindowShot(void* nsView, const char* path, int* w, int* h);
bool plugprobeShowFront(void* nsView);

// --- UI tree + input (snapshot/act) ---
// press returns 1 pressed / 0 unknown id / -1 press failed.
std::vector<PlugprobeAxNode> plugprobeAxDump(void* nsView);
int plugprobeAxPressById(const char* nodeId, void* nsView, double* cx,
                         double* cy);
// Raw screen-coordinate click (mouse space, top-left) for AX-empty
// custom-painted editors: grounded by a visible screenshot, flagged fragile.
bool plugprobeAxClickAt(double x, double y);
void plugprobeFocusWindow(void* nsView);
bool plugprobeAxDragAt(double x, double y, double dx, double dy);
bool plugprobeAxTypeText(const char* text);
void plugprobePumpApp(double seconds);
#if __APPLE__
#include <ApplicationServices/ApplicationServices.h>  // AXIsProcessTrusted:
// ad-hoc signatures change hash every rebuild, which invalidates the grant —
// HID drag/type must check this at runtime.
#endif

// --- Screen recording (opt-in `video` on render; macOS AVCapture only) ---
// Opaque handle; start captures the display region behind the editor window
// (needs it on-screen + the Screen Recording grant). finish stops capture
// and muxes wavIn (the rendered take) as the audio track into mp4Out, so
// what you see produced exactly what you hear. Diagnostic-only: failures
// return false/empty, never throw; the audio take is unaffected.
void* plugprobeScreenRecStart(void* nsView, std::string& err);
bool plugprobeScreenRecFinish(void* rec, const char* wavIn, const char* mp4Out,
                              std::string& err);
