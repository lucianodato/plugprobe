// GPL-3.0-or-later — Copyright (c) 2026 Luciano Dato — plugprobe
// plugprobe_os_linux.cpp: Linux OS backend (X11). Input: XTest (US-layout ASCII
// typing). Capture: XGetImage encoded to PNG via JUCE. Without an X display
// (Wayland-only, headless) every capability is off and callers fail loud with
// NO_OS_DRIVER. Not yet: UI tree (AT-SPI) and screen recording.
#include <juce_gui_basics/juce_gui_basics.h>

#include "plugprobe_os.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/extensions/XTest.h>

#include <cstdint>
#include <cstring>
#include <set>

namespace {
Display* xdisplay() {
  static Display* d = XOpenDisplay(nullptr);  // one connection for the process
  return d;
}

Window windowOf(void* hv) { return (Window)(uintptr_t)hv; }

void pumpMs(int ms) { juce::MessageManager::getInstance()->runDispatchLoopUntil(ms); }

// 1 ok / 0 fail / -1 blank (uniform grab, e.g. a GL-only or unmapped editor).
int saveWindowShot(void* hv, const char* path, int* w, int* h) {
  Display* d = xdisplay();
  if (!d) return 0;
  XWindowAttributes a;
  if (!XGetWindowAttributes(d, windowOf(hv), &a) || a.width <= 0 ||
      a.height <= 0)
    return 0;
  if (w) *w = a.width;
  if (h) *h = a.height;
  if (a.map_state != IsViewable) return 0;  // unmapped windows have no pixels
  XImage* xi = XGetImage(d, windowOf(hv), 0, 0, a.width, a.height, AllPlanes,
                         ZPixmap);
  if (!xi) return 0;
  if (xi->bits_per_pixel != 32 || xi->red_mask != 0xff0000 ||
      xi->green_mask != 0xff00 || xi->blue_mask != 0xff) {
    XDestroyImage(xi);
    return 0;  // only the 32-bit TrueColor layout every desktop X server uses
  }
  int W = xi->width, H = xi->height;
  std::set<unsigned long> seen;
  for (int y = 0; y < H; y += 7)
    for (int x = 0; x < W; x += 7) seen.insert(XGetPixel(xi, x, y));
  if (seen.size() <= 4) {
    XDestroyImage(xi);
    return -1;
  }
  juce::Image img(juce::Image::ARGB, W, H, false);
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x) {
      unsigned long p = XGetPixel(xi, x, y);
      img.setPixelAt(x, y,
                     juce::Colour((juce::uint8)((p >> 16) & 0xff),
                                  (juce::uint8)((p >> 8) & 0xff),
                                  (juce::uint8)(p & 0xff)));
    }
  XDestroyImage(xi);
  juce::File f(path);
  f.getParentDirectory().createDirectory();
  juce::FileOutputStream os(f);
  if (!os.openedOk()) return 0;
  os.setPosition(0);
  os.truncate();
  juce::PNGImageFormat png;
  return png.writeImageToStream(img, os) ? 1 : 0;
}

bool xtestAvailable(Display* d) {
  int ev = 0, er = 0, maj = 0, min = 0;
  return d && XTestQueryExtension(d, &ev, &er, &maj, &min);
}

void sleepMs(int ms) { juce::Thread::sleep(ms); }

bool typeAscii(Display* d, unsigned char c) {
  bool upper = c >= 'A' && c <= 'Z';
  if (!upper && !(c >= 0x20 && c < 0x7f)) return false;  // ASCII only
  KeySym ks = upper ? (KeySym)(c - 'A' + 'a') : (KeySym)c;
  KeyCode kc = XKeysymToKeycode(d, ks);
  KeyCode sh = upper ? XKeysymToKeycode(d, XK_Shift_L) : 0;
  if (kc == 0 || (upper && sh == 0)) return false;
  if (sh) XTestFakeKeyEvent(d, sh, True, CurrentTime);
  XTestFakeKeyEvent(d, kc, True, CurrentTime);
  XTestFakeKeyEvent(d, kc, False, CurrentTime);
  if (sh) XTestFakeKeyEvent(d, sh, False, CurrentTime);
  XFlush(d);
  sleepMs(20);
  return true;
}
}  // namespace

PlugprobeOsCaps plugprobeOsCaps() {
  Display* d = xdisplay();
  return {d != nullptr, false, xtestAvailable(d), false};
}
bool plugprobeInputGranted() { return true; }

int plugprobeSaveNSViewShot(void* nsView, const char* path, int* w, int* h) {
  return saveWindowShot(nsView, path, w, h);
}
int plugprobeSaveWindowShot(void* nsView, const char* path, int* w, int* h) {
  return saveWindowShot(nsView, path, w, h);
}

bool plugprobeShowFront(void* hv) {
  Display* d = xdisplay();
  if (!d) return false;
  XMapRaised(d, windowOf(hv));
  XFlush(d);
  pumpMs(300);
  XWindowAttributes a;
  return XGetWindowAttributes(d, windowOf(hv), &a) && a.map_state == IsViewable;
}

std::vector<PlugprobeAxNode> plugprobeAxDump(void*) { return {}; }  // no AT-SPI yet
int plugprobeAxPressById(const char*, void*, double*, double*) { return -1; }
int plugprobeAxSetValueById(const char*, void*, double, double*) { return -1; }

bool plugprobeAxClickAt(double x, double y) {
  Display* d = xdisplay();
  if (!d) return false;
  XTestFakeMotionEvent(d, -1, (int)x, (int)y, CurrentTime);
  XTestFakeButtonEvent(d, 1, True, CurrentTime);
  XFlush(d);
  sleepMs(60);
  XTestFakeButtonEvent(d, 1, False, CurrentTime);
  XFlush(d);
  pumpMs(400);
  return true;
}

bool plugprobeAxDragAt(double x, double y, double dx, double dy) {
  Display* d = xdisplay();
  if (!d) return false;
  XTestFakeMotionEvent(d, -1, (int)x, (int)y, CurrentTime);
  XTestFakeButtonEvent(d, 1, True, CurrentTime);
  XFlush(d);
  for (int i = 1; i <= 8; ++i) {
    XTestFakeMotionEvent(d, -1, (int)(x + dx * i / 8), (int)(y + dy * i / 8),
                         CurrentTime);
    XFlush(d);
    sleepMs(8);
  }
  XTestFakeButtonEvent(d, 1, False, CurrentTime);
  XFlush(d);
  pumpMs(400);
  return true;
}

bool plugprobeAxTypeText(const char* text) {
  Display* d = xdisplay();
  if (!d) return false;
  for (const char* p = text; *p; ++p)
    if (!typeAscii(d, (unsigned char)*p)) return false;
  pumpMs(300);
  return true;
}

void plugprobeFocusWindow(void* hv) {
  Display* d = xdisplay();
  if (!d) return;
  XRaiseWindow(d, windowOf(hv));
  XSetInputFocus(d, windowOf(hv), RevertToParent, CurrentTime);
  XFlush(d);
  pumpMs(300);
}

void plugprobePumpApp(double seconds) { pumpMs((int)(seconds * 1000.0)); }

void plugprobeTryActivate() {}

// Not implemented yet (PipeWire / frame-grab encoder, later step).
void* plugprobeScreenRecStart(void*, std::string&) { return nullptr; }
void plugprobeScreenRecGrab(void*) {}
bool plugprobeScreenRecFinish(void*, const char*, const char*, std::string&) {
  return false;
}
