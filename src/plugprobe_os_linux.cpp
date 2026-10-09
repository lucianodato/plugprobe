// GPL-3.0-or-later — Copyright (c) 2026 Luciano Dato — plugprobe
// plugprobe_os_linux.cpp: Linux OS backend (X11). Input: XTest (US-layout ASCII
// typing). Frame grab: XGetImage. PNG and AVI encoding are shared (capture.cpp).
// Without an X display (Wayland-only, headless) every capability is off and
// callers fail loud with NO_OS_DRIVER. Not yet: UI tree (AT-SPI).
#include <juce_gui_basics/juce_gui_basics.h>

#include "capture.h"
#include "plugprobe_os.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/extensions/XTest.h>

#include <cstdint>
#include <mutex>

namespace {
Display* xdisplay() {
  static Display* d = XOpenDisplay(nullptr);  // one connection for the process
  return d;
}

Window windowOf(void* hv) { return (Window)(uintptr_t)hv; }

std::mutex xErrorTrapMutex;
class ScopedXErrorTrap {
 public:
  explicit ScopedXErrorTrap(Display* d) : display(d) {
    XSync(display, False);
    std::lock_guard<std::mutex> lock(xErrorTrapMutex);
    previous = XSetErrorHandler(handle);
    active = this;
  }
  ~ScopedXErrorTrap() {
    XSync(display, False);
    std::lock_guard<std::mutex> lock(xErrorTrapMutex);
    if (active == this) {
      XSetErrorHandler(previous);
      active = nullptr;
    }
  }
  bool failed() {
    XSync(display, False);
    std::lock_guard<std::mutex> lock(xErrorTrapMutex);
    return errorCode != 0;
  }

 private:
  static int handle(Display* d, XErrorEvent* error) {
    XErrorHandler prior = nullptr;
    {
      std::lock_guard<std::mutex> lock(xErrorTrapMutex);
      if (active != nullptr && active->display == d) {
        active->errorCode = error->error_code;
        return 0;
      }
      if (active != nullptr) prior = active->previous;
    }
    return prior != nullptr ? prior(d, error) : 0;
  }

  static ScopedXErrorTrap* active;
  Display* display;
  XErrorHandler previous = nullptr;
  int errorCode = 0;
};
ScopedXErrorTrap* ScopedXErrorTrap::active = nullptr;

void pumpMs(int ms) { juce::MessageManager::getInstance()->runDispatchLoopUntil(ms); }

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
  return {d != nullptr, false, xtestAvailable(d), d != nullptr};
}
bool plugprobeInputGranted() { return true; }

bool plugprobeGrabFrame(void* hv, PlugprobeRgbFrame& out) {
  Display* d = xdisplay();
  if (!d) return false;
  ScopedXErrorTrap errors(d);
  XWindowAttributes a;
  if (!XGetWindowAttributes(d, windowOf(hv), &a) || a.width <= 0 ||
      a.height <= 0 || errors.failed())
    return false;
  if (a.map_state != IsViewable) return false;  // unmapped windows have no pixels
  XImage* xi = XGetImage(d, windowOf(hv), 0, 0, a.width, a.height, AllPlanes,
                         ZPixmap);
  if (!xi || errors.failed()) {
    if (xi != nullptr) XDestroyImage(xi);
    return false;
  }
  if (xi->bits_per_pixel != 32 || xi->red_mask != 0xff0000 ||
      xi->green_mask != 0xff00 || xi->blue_mask != 0xff) {
    XDestroyImage(xi);
    return false;  // only the 32-bit TrueColor layout every desktop X server uses
  }
  out.w = xi->width;
  out.h = xi->height;
  out.rgb.resize((size_t)out.w * out.h * 3);
  for (int y = 0; y < out.h; ++y)
    for (int x = 0; x < out.w; ++x) {
      unsigned long p = XGetPixel(xi, x, y);
      unsigned char* dst = out.rgb.data() + ((size_t)y * out.w + x) * 3;
      dst[0] = (unsigned char)((p >> 16) & 0xff);
      dst[1] = (unsigned char)((p >> 8) & 0xff);
      dst[2] = (unsigned char)(p & 0xff);
    }
  XDestroyImage(xi);
  return true;
}

int plugprobeSaveNSViewShot(void* nsView, const char* path, int* w, int* h) {
  return pp::saveShotPng(nsView, path, w, h);
}
int plugprobeSaveWindowShot(void* nsView, const char* path, int* w, int* h) {
  return pp::saveShotPng(nsView, path, w, h);
}

bool plugprobeShowFront(void* hv) {
  Display* d = xdisplay();
  if (!d) return false;
  ScopedXErrorTrap errors(d);
  XMapRaised(d, windowOf(hv));
  XFlush(d);
  pumpMs(300);
  XWindowAttributes a;
  return XGetWindowAttributes(d, windowOf(hv), &a) &&
         a.map_state == IsViewable && !errors.failed();
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
