// GPL-3.0-or-later — Copyright (c) 2026 Luciano Dato — plugprobe
// plugprobe_os_mac.mm: macOS OS backend (real). The plugin window lives in
// THIS process (we host the editor), so its AX tree is ours to walk and its
// controls ours to press — no cross-app trust needed. Native frameworks only.
// Capture: JUCE's createComponentSnapshot skips native (heavyweight) plugin
// views, so the NSView is asked to render itself into a bitmap rep.
#import <Cocoa/Cocoa.h>

#include "plugprobe_os.h"

#include <ApplicationServices/ApplicationServices.h>

#include <cmath>
#include <cstdio>
#include <errno.h>
#include <objc/runtime.h>
#include <spawn.h>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace {
// Every copyVal caller owns the result and must CFRelease it.
bool copyVal(AXUIElementRef el, CFStringRef attr, CFTypeRef* out) {
  *out = nullptr;
  return AXUIElementCopyAttributeValue(el, attr, out) == kAXErrorSuccess;
}
std::string cfStr(CFTypeRef v) {
  if (v == nullptr || CFGetTypeID(v) != CFStringGetTypeID()) return {};
  char buf[1024];
  if (!CFStringGetCString((CFStringRef)v, buf, sizeof(buf),
                          kCFStringEncodingUTF8))
    return {};
  return buf;
}
std::string attrStr(AXUIElementRef el, CFStringRef attr) {
  CFTypeRef v = nullptr;
  std::string s;
  if (copyVal(el, attr, &v)) {
    s = cfStr(v);
    CFRelease(v);
  }
  return s;
}
bool attrBool(AXUIElementRef el, CFStringRef attr, bool dflt) {
  CFTypeRef v = nullptr;
  bool b = dflt;
  if (copyVal(el, attr, &v)) {
    if (v != nullptr && CFGetTypeID(v) == CFBooleanGetTypeID())
      b = CFBooleanGetValue((CFBooleanRef)v);
    CFRelease(v);
  }
  return b;
}
std::string attrValStr(AXUIElementRef el) {
  CFTypeRef v = nullptr;
  std::string s;
  if (!copyVal(el, kAXValueAttribute, &v)) return s;
  if (v != nullptr) {
    if (CFGetTypeID(v) == CFStringGetTypeID()) {
      s = cfStr(v);
    } else if (CFGetTypeID(v) == CFBooleanGetTypeID()) {
      s = CFBooleanGetValue((CFBooleanRef)v) ? "true" : "false";
    } else if (CFGetTypeID(v) == CFNumberGetTypeID()) {
      double d = 0;
      CFNumberGetValue((CFNumberRef)v, kCFNumberDoubleType, &d);
      char buf[64];
      if (d == (long long)d)
        snprintf(buf, sizeof(buf), "%lld", (long long)d);
      else
        snprintf(buf, sizeof(buf), "%g", d);
      s = buf;
    }
    CFRelease(v);
  }
  return s;
}
bool ptOf(AXUIElementRef el, double* x, double* y) {
  CFTypeRef v = nullptr;
  if (!copyVal(el, kAXPositionAttribute, &v)) return false;
  CGPoint p;
  bool ok = v != nullptr && AXValueGetValue((AXValueRef)v,
                                             (AXValueType)kAXValueCGPointType,
                                             &p);
  if (ok) {
    *x = p.x;
    *y = p.y;
  }
  if (v) CFRelease(v);
  return ok;
}
bool sizeOf(AXUIElementRef el, double* w, double* h) {
  CFTypeRef v = nullptr;
  if (!copyVal(el, kAXSizeAttribute, &v)) return false;
  CGSize s;
  bool ok = v != nullptr && AXValueGetValue((AXValueRef)v,
                                             (AXValueType)kAXValueCGSizeType,
                                             &s);
  if (ok) {
    *w = s.width;
    *h = s.height;
  }
  if (v) CFRelease(v);
  return ok;
}
// Our window: the app hosts exactly one (the editor host). If several exist,
// match by frame (AX space is main-display top-left based).
AXUIElementRef findOurWindow(NSWindow* w) {
  AXUIElementRef app = AXUIElementCreateApplication(getpid());
  CFArrayRef wins = nullptr;
  AXUIElementRef found = nullptr;
  if (AXUIElementCopyAttributeValues(app, kAXWindowsAttribute, 0, 32, &wins) ==
          kAXErrorSuccess &&
      wins != nullptr) {
    if (CFArrayGetCount(wins) == 1) {
      found = (AXUIElementRef)CFArrayGetValueAtIndex(wins, 0);
      if (found) CFRetain(found);
    } else if (w != nullptr) {
      NSRect f = [w frame];
      CGFloat sh = [[NSScreen mainScreen] frame].size.height;
      double ax = f.origin.x, ay = sh - (f.origin.y + f.size.height);
      for (CFIndex i = 0; i < CFArrayGetCount(wins); ++i) {
        auto* e = (AXUIElementRef)CFArrayGetValueAtIndex(wins, i);
        double x = 0, y = 0, sw = 0, sh2 = 0;
        if (ptOf(e, &x, &y) && sizeOf(e, &sw, &sh2) &&
            fabs(x - ax) < 8 && fabs(y - ay) < 8 &&
            fabs(sw - f.size.width) < 8 && fabs(sh2 - f.size.height) < 8) {
          found = e;
          CFRetain(found);
          break;
        }
      }
    }
    CFRelease(wins);
  }
  CFRelease(app);
  return found;  // retained or null; caller releases
}
struct Found {
  PlugprobeAxNode node;
  AXUIElementRef el = nullptr;  // retained; released by owner
};
void walkEl(AXUIElementRef el, std::vector<Found>& out, int depth,
            int* counter) {
  if (depth > 24 || out.size() > 2000 || el == nullptr) return;
  Found f;
  f.node.role = attrStr(el, kAXRoleAttribute);
  std::string title = attrStr(el, kAXTitleAttribute);
  std::string desc = attrStr(el, kAXDescriptionAttribute);
  f.node.name = !title.empty() ? title : desc;
  f.node.enabled = attrBool(el, kAXEnabledAttribute, true);
  f.node.value = attrValStr(el);
  ptOf(el, &f.node.x, &f.node.y);
  sizeOf(el, &f.node.w, &f.node.h);
  // Stable id: role + human name, index fallback, dedupe suffix.
  std::string base =
      (f.node.role.empty() ? "AXUnknown" : f.node.role) + ":" +
      (f.node.name.empty() ? ("#" + std::to_string(++(*counter)))
                            : f.node.name);
  f.node.id = base;
  int dup = 1;
  for (auto& o : out)
    if (o.node.id == f.node.id) f.node.id = base + "#" + std::to_string(++dup);
  f.el = el;
  CFRetain(f.el);
  out.push_back(f);
  CFArrayRef kids = nullptr;
  if (AXUIElementCopyAttributeValues(el, kAXChildrenAttribute, 0, 500,
                                     &kids) == kAXErrorSuccess &&
      kids != nullptr) {
    for (CFIndex i = 0; i < CFArrayGetCount(kids); ++i)
      walkEl((AXUIElementRef)CFArrayGetValueAtIndex(kids, i), out, depth + 1,
             counter);
    CFRelease(kids);
  }
}
void releaseAll(std::vector<Found>& v) {
  for (auto& f : v)
    if (f.el) CFRelease(f.el);
  v.clear();
}
// Dump starting at our window (found via the hosted NSView).
std::vector<Found> collect(void* nsView) {
  std::vector<Found> v;
  NSView* view = (NSView*)nsView;
  if (view == nullptr) return v;
  AXUIElementRef win = findOurWindow([view window]);
  if (win == nullptr) return v;
  int counter = 0;
  walkEl(win, v, 0, &counter);
  CFRelease(win);
  return v;
}
}  // namespace

std::vector<PlugprobeAxNode> plugprobeAxDump(void* nsView) {
  auto found = collect(nsView);
  std::vector<PlugprobeAxNode> nodes;
  nodes.reserve(found.size());
  for (auto& f : found) nodes.push_back(f.node);
  releaseAll(found);
  return nodes;
}
// 1 pressed / 0 unknown id / -1 press failed. Center out on success.
int plugprobeAxPressById(const char* nodeId, void* nsView, double* cx,
                     double* cy) {
  if (nodeId == nullptr) return 0;
  auto found = collect(nsView);
  int rc = 0;
  for (auto& f : found) {
    if (f.node.id == nodeId) {
      rc = AXUIElementPerformAction(f.el, kAXPressAction) == kAXErrorSuccess
               ? 1
               : -1;
      if (rc == 1 && cx && cy) {
        *cx = f.node.x + f.node.w / 2;
        *cy = f.node.y + f.node.h / 2;
      }
      break;
    }
  }
  releaseAll(found);
  return rc;
}
// Pump AppKit event delivery. A console tool never runs [NSApp run], so
// CFRunLoop pumping alone leaves posted HID events stranded in our own
// queue (monitors see nothing, controls never move). Drain manually.
void plugprobePumpApp(double seconds) {
  [[NSRunLoop mainRunLoop] runUntilDate:[NSDate
      dateWithTimeIntervalSinceNow:seconds]];
  NSDate* end = [NSDate dateWithTimeIntervalSinceNow:seconds];
  NSEvent* ev;
  while ((ev = [NSApp nextEventMatchingMask:NSEventMaskAny
                                  untilDate:end
                                     inMode:NSDefaultRunLoopMode
                                    dequeue:YES])) {
    [NSApp sendEvent:ev];
  }
  [NSApp updateWindows];
}
// Bring our hosted window key before HID events post: the first click on an
// inactive window only activates it (no click-through for buttons), so an
// unfocused clickAt would toggle nothing. API activation + drain, no cursor
// movement, so the real click is the first one the control sees.
void plugprobeFocusWindow(void* nsView) {
  NSView* v = (NSView*)nsView;
  if (v == nil) return;
  NSWindow* w = [v window];
  if (w == nil) return;
  [NSApp activateIgnoringOtherApps:YES];
  [w makeKeyAndOrderFront:nil];
  plugprobePumpApp(0.3);
}
// Raw screen-coordinate click (AX space == mouse space, top-left) for AX-empty
// custom-painted editors. Ends with an AppKit drain so the click lands.
bool plugprobeAxClickAt(double x, double y) {
  CGPoint p = CGPointMake(x, y);
  CGEventRef down = CGEventCreateMouseEvent(nullptr, kCGEventLeftMouseDown, p,
                                            kCGMouseButtonLeft);
  if (!down) return false;
  CGEventPost(kCGHIDEventTap, down);
  CFRelease(down);
  usleep(60000);
  CGEventRef up =
      CGEventCreateMouseEvent(nullptr, kCGEventLeftMouseUp, p, kCGMouseButtonLeft);
  if (!up) return false;
  CGEventPost(kCGHIDEventTap, up);
  CFRelease(up);
  plugprobePumpApp(0.4);
  return true;
}
// HID-level drag from a screen point (AX space == mouse space, top-left).
// Ends with an AppKit drain so the gesture is delivered before returning.
bool plugprobeAxDragAt(double x, double y, double dx, double dy) {
  CGPoint from = CGPointMake(x, y), to = CGPointMake(x + dx, y + dy);
  CGEventRef down = CGEventCreateMouseEvent(nullptr, kCGEventLeftMouseDown,
                                            from, kCGMouseButtonLeft);
  if (!down) return false;
  CGEventPost(kCGHIDEventTap, down);
  CFRelease(down);
  const int steps = 8;
  for (int i = 1; i <= steps; ++i) {
    CGPoint p = CGPointMake(from.x + (to.x - from.x) * i / steps,
                            from.y + (to.y - from.y) * i / steps);
    CGEventRef mv = CGEventCreateMouseEvent(nullptr, kCGEventLeftMouseDragged,
                                            p, kCGMouseButtonLeft);
    if (mv) {
      CGEventPost(kCGHIDEventTap, mv);
      CFRelease(mv);
    }
    usleep(8000);
  }
  CGEventRef up = CGEventCreateMouseEvent(nullptr, kCGEventLeftMouseUp, to,
                                          kCGMouseButtonLeft);
  if (up) {
    CGEventPost(kCGHIDEventTap, up);
    CFRelease(up);
  }
  plugprobePumpApp(0.4);
  return true;
}
bool plugprobeAxTypeText(const char* text) {
  if (text == nullptr || *text == '\0') return false;
  NSString* s = [NSString stringWithUTF8String:text];
  NSUInteger n = [s length];
  std::vector<UniChar> buf(n);
  [s getCharacters:buf.data() range:NSMakeRange(0, n)];
  CGEventRef down = CGEventCreateKeyboardEvent(nullptr, (CGKeyCode)0, true);
  bool ok = down != nullptr;
  if (ok) {
    CGEventKeyboardSetUnicodeString(down, (UniCharCount)n, buf.data());
    CGEventPost(kCGHIDEventTap, down);
    CFRelease(down);
  }
  CGEventRef up = CGEventCreateKeyboardEvent(nullptr, (CGKeyCode)0, false);
  if (up) {
    CGEventPost(kCGHIDEventTap, up);
    CFRelease(up);
  } else {
    ok = false;
  }
  if (ok) plugprobePumpApp(0.3);
  return ok;
}
// --- Editor capture (opt-in `shot` PNGs) ---
// JUCE's createComponentSnapshot skips native (heavyweight) plugin views, so
// ask the NSView to render itself into a bitmap rep: no window needed.
// Force the host window front. plugprobe is a background CLI tool whose windows
// AppKit would otherwise never order in; headed mode exists to be watched,
// so this promotes the process to a regular app and steals focus openly
// (a Dock icon appears for the run).
// Returns whether pixels can really reach the screen.
bool plugprobeShowFront(void* nsView) {
  NSView* v = (NSView*)nsView;
  NSWindow* w = [v window];
  if (w == nil) return false;
  // A console tool never finishes launching on its own, and without it the
  // WindowServer never composites its windows (isVisible lies).
  [NSApp finishLaunching];
  if ([NSApp activationPolicy] != NSApplicationActivationPolicyRegular &&
      ![NSApp setActivationPolicy:NSApplicationActivationPolicyRegular])
    return false;
  [NSApp activateIgnoringOtherApps:YES];
  [w makeKeyAndOrderFront:nil];
  // Above normal windows: a Terminal/VSCode window over the same spot would
  // otherwise swallow HID clicks meant for the plugin (AXPress is immune,
  // CGEvent is not). Headed mode only; the window dies with the process.
  [w setLevel:NSFloatingWindowLevel];
  [w orderFrontRegardless];
  return [NSApp activationPolicy] == NSApplicationActivationPolicyRegular &&
         [w isVisible];
}

// Returns 1 on success, 0 on failure, -1 when the view paints (near-)uniform
// black: some editors (async/Metal views) never render offscreen, and a
// black PNG masquerading as a screenshot is worse than an explicit signal.
// Nothing is written in the blank case.
// ponytail: byte-variance heuristic; a real UI (even a dark one) carries
// antialiased text/edges, so a <=4-distinct-bytes grab is not a UI.
static int saveRep(NSString* p, NSBitmapImageRep* rep, int* w, int* h) {
  unsigned char* px = [rep bitmapData];
  long rowBytes = [rep bytesPerRow];
  long rows = [rep pixelsHigh];
  if (px == NULL || rowBytes < 1 || rows < 1) return 0;
  bool seen[256] = {false};
  int distinct = 0;
  long total = rowBytes * rows;
  long stride = total > 50000 ? total / 50000 : 1;
  for (long i = 0; i < total && distinct <= 4; i += stride) {
    if (!seen[px[i]]) {
      seen[px[i]] = true;
      ++distinct;
    }
  }
  if (distinct <= 4) return -1;
  NSData* png =
      [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
  if (png == nil) return 0;
  [[NSFileManager defaultManager]
      createDirectoryAtPath:[p stringByDeletingLastPathComponent]
          withIntermediateDirectories:YES
                           attributes:nil
                                error:nil];
  if (w != nullptr) *w = (int)[rep pixelsWide];
  if (h != nullptr) *h = (int)[rep pixelsHigh];
  return [png writeToFile:p atomically:YES];
}
// Composited window grab (WindowServer pixels): works for Metal/async editors
// whose NSView bitmap never paints. Needs the window on-screen; returns
// 0 (no window / not visible), -1 (no pixels or blank), 1 ok.
// ponytail: shells out to /usr/sbin/screencapture — CGWindowList capture is
// obsoleted in macOS 15 (ScreenCaptureKit is async-only overkill here), and
// the system tool owns the permission prompt + compositing. Diagnostic path
// only, never the audio thread.
int plugprobeSaveWindowShot(void* nsView, const char* path, int* w, int* h) {
  if (nsView == nullptr || path == nullptr) return 0;
  NSWindow* win = [(NSView*)nsView window];
  if (win == nil || ![win isVisible]) return 0;
  NSString* wid = [NSString stringWithFormat:@"%lld", (long long)[win windowNumber]];
  NSString* p = [NSString stringWithUTF8String:path];
  pid_t pid = 0;
  const char* argv[] = {"/usr/sbin/screencapture", "-l", [wid UTF8String],
                        "-x", [p UTF8String], nullptr};
  extern char** environ;
  if (posix_spawn(&pid, argv[0], nullptr, nullptr, (char* const*)argv,
                  environ) != 0)
    return 0;
  int st = 0;
  while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {
  }
  if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) return -1;  // e.g. denied: no pixels
  NSBitmapImageRep* rep =
      (NSBitmapImageRep*)[NSBitmapImageRep imageRepWithContentsOfFile:p];
  if (rep == nil) return -1;
  unsigned char* px = [rep bitmapData];
  long total = (long)[rep bytesPerRow] * (long)[rep pixelsHigh];
  if (px == NULL || total < 1) {
    [[NSFileManager defaultManager] removeItemAtPath:p error:nil];
    return -1;
  }
  bool seen[256] = {false};
  int distinct = 0;
  long stride = total > 50000 ? total / 50000 : 1;
  for (long i = 0; i < total && distinct <= 4; i += stride) {
    if (!seen[px[i]]) {
      seen[px[i]] = true;
      ++distinct;
    }
  }
  if (distinct <= 4) {
    [[NSFileManager defaultManager] removeItemAtPath:p error:nil];
    return -1;
  }
  if (w != nullptr) *w = (int)[rep pixelsWide];
  if (h != nullptr) *h = (int)[rep pixelsHigh];
  return 1;
}
int plugprobeSaveNSViewShot(void* nsView, const char* path, int* w, int* h) {
  if (nsView == nullptr || path == nullptr) return 0;
  NSView* v = (NSView*)nsView;
  NSRect b = [v bounds];
  if (b.size.width < 1 || b.size.height < 1) return 0;
  // Let async view content settle, then force a synchronous paint: hidden
  // windows never paint on their own, which is why plain grabs come out black.
  [[NSRunLoop mainRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.5]];
  [v setNeedsDisplay:YES];
  [v displayIfNeededIgnoringOpacity];
  NSBitmapImageRep* rep = [v bitmapImageRepForCachingDisplayInRect:b];
  if (rep == nil) return 0;
  [v cacheDisplayInRect:b toBitmapImageRep:rep];
  NSString* p = [NSString stringWithUTF8String:path];
  int rc = saveRep(p, rep, NULL, NULL);
  if (w != nullptr) *w = (int)b.size.width;
  if (h != nullptr) *h = (int)b.size.height;
  if (rc == 1) return 1;
  if (rc == 0) return 0;
  // Blank view grab: custom-painted (Metal/async) editors never paint into a
  // bitmap. Fall back to composited window pixels when on-screen, so a
  // visible screenshot can still ground coordinate clicks. Offscreen stays
  // an explicit blank (no stale PNG).
  if ([[v window] isVisible]) {
    int wr = plugprobeSaveWindowShot(nsView, path, w, h);
    if (wr == 1) return 1;
  }
  return -1;
}
