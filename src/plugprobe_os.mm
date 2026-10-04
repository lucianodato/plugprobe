// GPL-3.0-or-later — Copyright (c) 2026 Luciano Dato — plugprobe
// plugprobe_os.mm: macOS-only OS driver (M1). The plugin window lives in THIS
// process (we host the editor), so its AX tree is ours to walk and its
// controls ours to press — no cross-app trust needed. Native frameworks only.
#import <Cocoa/Cocoa.h>

#include <cstdio>
#include <objc/runtime.h>

#include <ApplicationServices/ApplicationServices.h>

#include <cmath>
#include <string>
#include <unistd.h>
#include <vector>

struct PlugprobeAxNode {
  std::string id, role, name;
  bool enabled = true;
  std::string value;
  double x = 0, y = 0, w = 0, h = 0;
};

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