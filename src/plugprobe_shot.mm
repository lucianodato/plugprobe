// MIT License — Copyright (c) 2026 Luciano Dato — plugprobe
// plugprobe_shot.mm: macOS-only offscreen NSView capture for opt-in `shot` PNGs.
// JUCE's createComponentSnapshot skips native (heavyweight) plugin views, so
// ask the NSView to render itself into a bitmap rep: no window needed.
#import <Cocoa/Cocoa.h>

#include <errno.h>
#include <spawn.h>
#include <sys/wait.h>

// Force the host window front. plugprobe is a background CLI tool whose windows
// AppKit would otherwise never order in; headed mode exists to be watched
// (like headed Playwright focusing its browser), so this promotes the process
// to a regular app and steals focus openly (a Dock icon appears for the run).
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
int plugprobeSaveWindowShot(void* nsView, const char* path, int* w, int* h);  // below
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
