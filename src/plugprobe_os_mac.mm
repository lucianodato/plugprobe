// GPL-3.0-or-later — Copyright (c) 2026 Luciano Dato — plugprobe
// plugprobe_os_mac.mm: macOS OS backend (real). The plugin window lives in
// THIS process (we host the editor), so its AX tree is ours to walk and its
// controls ours to press — no cross-app trust needed. Native frameworks only.
// Capture: JUCE's createComponentSnapshot skips native (heavyweight) plugin
// views, so the NSView is asked to render itself into a bitmap rep.
#import <Cocoa/Cocoa.h>
#import <AVFoundation/AVFoundation.h>
#import <CoreVideo/CoreVideo.h>
#import <ImageIO/ImageIO.h>

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
// 1 set / 0 unknown id / -1 set failed. Re-read value out in actual.
int plugprobeAxSetValueById(const char* nodeId, void* nsView, double value,
                            double* actual) {
  if (nodeId == nullptr) return 0;
  auto found = collect(nsView);
  int rc = 0;
  for (auto& f : found) {
    if (f.node.id == nodeId) {
      // Native units, as text: JUCE's bridge takes AXValue as a string
      // (setValueAsString); a CFNumber set is rejected.
      CFStringRef vs = CFStringCreateWithFormat(nullptr, nullptr, CFSTR("%.6f"),
                                               value);
      rc = AXUIElementSetAttributeValue(f.el, kAXValueAttribute, vs) ==
                   kAXErrorSuccess
               ? 1
               : -1;
      CFRelease(vs);
      if (rc == 1 && actual != nullptr) {
        CFTypeRef cur = nullptr;
        if (AXUIElementCopyAttributeValue(f.el, kAXValueAttribute, &cur) ==
                kAXErrorSuccess &&
            cur != nullptr) {
          if (CFGetTypeID(cur) == CFNumberGetTypeID())
            CFNumberGetValue((CFNumberRef)cur, kCFNumberDoubleType, actual);
          else if (CFGetTypeID(cur) == CFStringGetTypeID())
            *actual = [(NSString*)cur doubleValue];  // toll-free bridge
          CFRelease(cur);
        }
      }
      break;
    }
  }
  releaseAll(found);
  return rc;
}
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
// One-shot activation for headless sessions (CI): console tools may start
// with no GUI presence, leaving the AX window list empty despite the trust
// grant. Only called when the AX tree comes back empty — interactive runs
// never touch this (no dock bounce, no focus steal).
void plugprobeTryActivate() {
  [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
  [NSApp activateIgnoringOtherApps:YES];
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
// --- Screen recording (opt-in `video` on render) ---
// Frame-grab, not screen capture: the editor NSView renders itself into a
// bitmap per grab (same path as headless shots — works hidden, no Screen
// Recording grant, no visible window, no cursor). Grabs buffer as JPEGs
// (~20KB/frame, bounded memory); finish encodes H.264 via AVAssetWriter and
// appends the take WAV as the AAC track, padding the last frame to cover
// the audio. Offline renders play no device audio, so capturing system
// audio would record silence — the take file IS the soundtrack.
// ponytail: fixed ~15fps; enough to watch a slider sweep + FFT move.
// Retained-box macros: the .mm builds without ARC, so ObjC/CF objects in
// the C++ ScreenRec box need explicit retains (no-ops under ARC).
#if __has_feature(objc_arc)
#define PP_RETAINED(p) ((void*)CFBridgingRetain(p))
#define PP_BRIDGE(T, p) ((__bridge T)(p))
#else
#define PP_RETAINED(p) ((void*)CFRetain((CFTypeRef)(p)))
#define PP_BRIDGE(T, p) ((T)(p))
#endif
struct ScreenRec {
  void* view = nullptr;  // NSView, owned by the JUCE editor (not retained)
  std::vector<void*> frames;  // JPEG NSDatas, CFReleased in finish
  int w = 0, h = 0;           // even-sized encode dims from the first frame
  double lastGrab = 0;        // throttle stamp
};
// Render the view into a JPEG (same forced-paint path as headless shots).
// Null when the view has no size or won't paint (Metal/async editors).
static void* grabJpeg(NSView* v, int* w, int* h) {
  NSRect b = [v bounds];
  if (b.size.width < 2 || b.size.height < 2) return nullptr;
  [v setNeedsDisplay:YES];
  [v displayIfNeededIgnoringOpacity];
  NSBitmapImageRep* rep = [v bitmapImageRepForCachingDisplayInRect:b];
  if (rep == nil) return nullptr;
  [v cacheDisplayInRect:b toBitmapImageRep:rep];
  NSData* jpg = [rep representationUsingType:NSJPEGFileType
                                  properties:@{NSImageCompressionFactor: @0.7}];
  if (jpg == nil || [jpg length] == 0) return nullptr;
  int w2 = ((int)b.size.width) & ~1, h2 = ((int)b.size.height) & ~1;
  if (w2 < 2 || h2 < 2) return nullptr;
  if (w != nullptr) *w = w2;
  if (h != nullptr) *h = h2;
  return PP_RETAINED(jpg);
}
// JPEG -> 32ARGB pixel buffer at encode size (CoreGraphics converts).
static void* pxFromJpeg(void* jpg, int w, int h) {
  CGImageSourceRef src = CGImageSourceCreateWithData((CFDataRef)jpg, nullptr);
  if (src == nullptr) return nullptr;
  CGImageRef img = CGImageSourceCreateImageAtIndex(src, 0, nullptr);
  CFRelease(src);
  if (img == nullptr) return nullptr;
  CVPixelBufferRef px = nullptr;
  if (CVPixelBufferCreate(kCFAllocatorDefault, (size_t)w, (size_t)h,
                          kCVPixelFormatType_32ARGB, nullptr, &px) !=
      kCVReturnSuccess) {
    CGImageRelease(img);
    return nullptr;
  }
  CVPixelBufferLockBaseAddress(px, 0);
  CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
  CGContextRef ctx = CGBitmapContextCreate(
      CVPixelBufferGetBaseAddress(px), (size_t)w, (size_t)h, 8,
      CVPixelBufferGetBytesPerRow(px), cs,
      kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Big);
  if (ctx != nullptr) {
    CGContextDrawImage(ctx, CGRectMake(0, 0, w, h), img);
    CGContextRelease(ctx);
  }
  CGColorSpaceRelease(cs);
  CVPixelBufferUnlockBaseAddress(px, 0);
  CGImageRelease(img);
  return px;
}
void* plugprobeScreenRecStart(void* nsView, std::string& err) {
  if (nsView == nullptr) {
    err = "no-editor";
    return nullptr;
  }
  // No grant checks, no visibility requirement: grabs render the view
  // in-process (headless-safe, CI-safe).
  auto* r = new ScreenRec();
  r->view = nsView;
  r->lastGrab = CFAbsoluteTimeGetCurrent() - 1;  // first frame ASAP
  return r;
}
void plugprobeScreenRecGrab(void* rec) {
  auto* r = (ScreenRec*)rec;
  if (r == nullptr || r->view == nullptr) return;
  double now = CFAbsoluteTimeGetCurrent();
  if (now - r->lastGrab < 1.0 / 15.0) return;
  r->lastGrab = now;
  int w = 0, h = 0;
  void* jpg = grabJpeg((NSView*)r->view, &w, &h);
  if (jpg == nullptr) return;  // view won't paint; take is unaffected
  if (r->frames.empty()) {
    r->w = w;
    r->h = h;
  }
  if (w != r->w || h != r->h) {
    CFRelease(jpg);  // resize mid-take: keep the first size
    return;
  }
  r->frames.push_back(jpg);
}
bool plugprobeScreenRecFinish(void* rec, const char* wavIn, const char* mp4Out,
                              std::string& err) {
  std::unique_ptr<ScreenRec> r((ScreenRec*)rec);
  if (r == nullptr || wavIn == nullptr || mp4Out == nullptr) {
    err = "bad recorder";
    return false;
  }
  if (r->frames.empty()) {
    err = "no frames grabbed (view never painted)";
    return false;
  }
  NSString* mp4 =
      [NSString stringWithUTF8String:mp4Out] ?: @"";
  [[NSFileManager defaultManager]
      createDirectoryAtPath:[mp4 stringByDeletingLastPathComponent]
          withIntermediateDirectories:YES
                           attributes:nil
                                error:nil];
  [[NSFileManager defaultManager] removeItemAtPath:mp4 error:nil];
  NSError* e = nil;
  AVAssetWriter* wr = [[AVAssetWriter alloc] initWithURL:[NSURL fileURLWithPath:mp4]
                                               fileType:AVFileTypeMPEG4
                                                  error:&e];
  if (wr == nil) {
    err = "writer unavailable";
    return false;
  }
  AVAssetWriterInput* vin = [AVAssetWriterInput
      assetWriterInputWithMediaType:AVMediaTypeVideo
                     outputSettings:@{
                       AVVideoCodecKey: AVVideoCodecTypeH264,
                       AVVideoWidthKey: @(r->w),
                       AVVideoHeightKey: @(r->h)
                     }];
  vin.expectsMediaDataInRealTime = NO;
  AVAssetWriterInputPixelBufferAdaptor* ad =
      [AVAssetWriterInputPixelBufferAdaptor
          assetWriterInputPixelBufferAdaptorWithAssetWriterInput:vin
                                    sourcePixelBufferAttributes:@{
                                      (id)kCVPixelBufferPixelFormatTypeKey:
                                          @(kCVPixelFormatType_32ARGB)
                                    }];
  // Take WAV as the AAC track (reader passthrough -> encoder).
  AVURLAsset* aa = [AVURLAsset
      assetWithURL:[NSURL fileURLWithPath:[NSString stringWithUTF8String:wavIn]]];
// ponytail: sync track fetch; the async replacement is overkill for two
// local files muxed once per run.
#if __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
  AVAssetTrack* at = [[aa tracksWithMediaType:AVMediaTypeAudio] firstObject];
#if __clang__
#pragma clang diagnostic pop
#endif
  if (at == nil) {
    err = "take has no audio track";
    return false;
  }
  const AudioStreamBasicDescription* asbd = CMAudioFormatDescriptionGetStreamBasicDescription(
      (CMFormatDescriptionRef)[at.formatDescriptions firstObject]);
  if (asbd == nullptr) {
    err = "take audio format unreadable";
    return false;
  }
  AVAssetWriterInput* ain = [AVAssetWriterInput
      assetWriterInputWithMediaType:AVMediaTypeAudio
                     outputSettings:@{
                       AVFormatIDKey: @(kAudioFormatMPEG4AAC),
                       AVSampleRateKey: @(asbd->mSampleRate),
                       AVNumberOfChannelsKey: @(asbd->mChannelsPerFrame),
                       AVEncoderBitRateKey: @128000
                     }];
  ain.expectsMediaDataInRealTime = NO;
  AVAssetReader* rd = [AVAssetReader assetReaderWithAsset:aa error:&e];
  AVAssetReaderTrackOutput* rout =
      [AVAssetReaderTrackOutput assetReaderTrackOutputWithTrack:at
                                                 outputSettings:nil];
  if (rd == nil || ![wr canAddInput:vin] || ![wr canAddInput:ain] ||
      ![rd canAddOutput:rout]) {
    err = "writer/reader rejected inputs";
    return false;
  }
  [wr addInput:vin];
  [wr addInput:ain];
  [rd addOutput:rout];
  if (![wr startWriting]) {
    err = "writer would not start";
    return false;
  }
  [wr startSessionAtSourceTime:kCMTimeZero];
  auto waitReady = [](AVAssetWriterInput* in) {
    // Pump, don't sleep: media machinery needs main-runloop turns.
    for (int i = 0; i < 2000 && ![in isReadyForMoreMediaData]; ++i)
      [[NSRunLoop currentRunLoop]
          runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.005]];
    return [in isReadyForMoreMediaData];
  };
  // Audio FIRST: the muxer backpressures any track that runs ~1.5s ahead of
  // another starved one, so the complete (small) AAC track goes in before
  // the video stream starts. Video-first stalls at ~25 frames, every time.
  if (![rd startReading]) {
    err = "take reader would not start";
    return false;
  }
  while (rd.status == AVAssetReaderStatusReading) {
    CMSampleBufferRef sb = [rout copyNextSampleBuffer];
    if (sb == nullptr) break;
    if (waitReady(ain)) [ain appendSampleBuffer:sb];
    CFRelease(sb);
  }
  [ain markAsFinished];
  // Video: one presentation stamp per grabbed frame; pad the last frame to
  // cover the audio so A/V end together.
  CMTime fd = CMTimeMake(1, 15);
  size_t n = r->frames.size();
  auto appendPx = [&](void* jpg, CMTime t) {
    void* px = pxFromJpeg(jpg, r->w, r->h);
    if (px == nullptr) return false;
    if (waitReady(vin) &&
        [ad appendPixelBuffer:(CVPixelBufferRef)px withPresentationTime:t]) {
      CFRelease(px);
      return true;
    }
    CFRelease(px);
    return false;
  };
  for (size_t i = 0; i < n; ++i)
    if (!appendPx(r->frames[i], CMTimeMultiply(fd, (int64_t)i))) {
      err = "video append failed";
      return false;
    }
  for (int64_t i = (int64_t)n;
       CMTimeCompare(CMTimeMultiply(fd, i), aa.duration) < 0 &&
       i < (int64_t)n + 15 * 60;
       ++i)
    if (!appendPx(r->frames.back(), CMTimeMultiply(fd, i))) break;
  [vin markAsFinished];
  [wr finishWriting];
  for (void* p : r->frames) CFRelease(p);
  r->frames.clear();
  if (wr.status != AVAssetWriterStatusCompleted) {
    err = "video write failed";
    return false;
  }
  return true;
}

PlugprobeOsCaps plugprobeOsCaps() { return {true, true, true, true}; }
bool plugprobeInputGranted() { return AXIsProcessTrusted(); }
