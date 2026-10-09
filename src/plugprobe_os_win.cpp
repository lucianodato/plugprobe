// GPL-3.0-or-later — Copyright (c) 2026 Luciano Dato — plugprobe
// plugprobe_os_win.cpp: Windows OS backend. Tree: UI Automation (ElementFromHandle,
// Invoke / RangeValue patterns). Input: SendInput (mouse + unicode keys).
// Frame grab: PrintWindow(PW_RENDERFULLCONTENT). PNG and AVI encoding are shared
// (capture.cpp).
#include "plugprobe_os.h"

#include "capture.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <objbase.h>
#include <gdiplus.h>
#include <oleauto.h>
#include <UIAutomation.h>
#include <wrl/client.h>

#include <cstdio>
#include <map>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {
std::wstring toWide(const std::string& s) {
  if (s.empty()) return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
  std::wstring w(n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
  return w;
}

std::string toUtf8(const wchar_t* w, int n) {
  if (n <= 0) return {};
  int len = WideCharToMultiByte(CP_UTF8, 0, w, n, nullptr, 0, nullptr, nullptr);
  std::string s(len, '\0');
  WideCharToMultiByte(CP_UTF8, 0, w, n, s.data(), len, nullptr, nullptr);
  return s;
}

void ensureCom() {
  static const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  (void)hr;  // RPC_E_CHANGED_MODE just means the message thread already set it
}

const char* roleOf(CONTROLTYPEID ct) {
  switch (ct) {
    case UIA_ButtonControlTypeId: return "Button";
    case UIA_CheckBoxControlTypeId: return "CheckBox";
    case UIA_ComboBoxControlTypeId: return "ComboBox";
    case UIA_EditControlTypeId: return "Edit";
    case UIA_HyperlinkControlTypeId: return "Hyperlink";
    case UIA_ImageControlTypeId: return "Image";
    case UIA_ListControlTypeId: return "List";
    case UIA_ListItemControlTypeId: return "ListItem";
    case UIA_MenuControlTypeId: return "Menu";
    case UIA_MenuItemControlTypeId: return "MenuItem";
    case UIA_ProgressBarControlTypeId: return "ProgressBar";
    case UIA_RadioButtonControlTypeId: return "RadioButton";
    case UIA_ScrollBarControlTypeId: return "ScrollBar";
    case UIA_SliderControlTypeId: return "Slider";
    case UIA_SpinnerControlTypeId: return "Spinner";
    case UIA_TabControlTypeId: return "Tab";
    case UIA_TabItemControlTypeId: return "TabItem";
    case UIA_TextControlTypeId: return "Text";
    case UIA_ToolBarControlTypeId: return "ToolBar";
    case UIA_GroupControlTypeId: return "Group";
    case UIA_WindowControlTypeId: return "Window";
    case UIA_PaneControlTypeId: return "Pane";
    case UIA_ThumbControlTypeId: return "Thumb";
    default: return "Unknown";
  }
}

struct Walked {
  PlugprobeAxNode node;
  ComPtr<IUIAutomationElement> el;
};

PlugprobeAxNode describe(IUIAutomationElement* el) {
  PlugprobeAxNode n;
  CONTROLTYPEID ct = 0;
  el->get_CurrentControlType(&ct);
  n.role = roleOf(ct);
  BSTR name = nullptr;
  if (SUCCEEDED(el->get_CurrentName(&name)) && name)
    n.name = toUtf8(name, (int)SysStringLen(name));
  SysFreeString(name);
  BOOL enabled = TRUE;
  el->get_CurrentIsEnabled(&enabled);
  n.enabled = enabled != FALSE;
  RECT r{};
  if (SUCCEEDED(el->get_CurrentBoundingRectangle(&r))) {
    n.x = r.left;
    n.y = r.top;
    n.w = r.right - r.left;
    n.h = r.bottom - r.top;
  }
  ComPtr<IUIAutomationRangeValuePattern> rp;
  if (SUCCEEDED(el->GetCurrentPatternAs(UIA_RangeValuePatternId,
                                        IID_PPV_ARGS(&rp)))) {
    double v = 0;
    if (SUCCEEDED(rp->get_CurrentValue(&v))) {
      char buf[32];
      snprintf(buf, sizeof buf, "%g", v);
      n.value = buf;
    }
  } else {
    ComPtr<IUIAutomationValuePattern> vp;
    if (SUCCEEDED(el->GetCurrentPatternAs(UIA_ValuePatternId,
                                          IID_PPV_ARGS(&vp)))) {
      BSTR v = nullptr;
      if (SUCCEEDED(vp->get_CurrentValue(&v)) && v)
        n.value = toUtf8(v, (int)SysStringLen(v));
      SysFreeString(v);
    }
  }
  return n;
}

// Control view, depth <= 24, <= 2000 nodes, <= 500 children per parent (same
// bounds as the macOS walker).
void walkEl(IUIAutomationTreeWalker* tw, IUIAutomationElement* el, int depth,
            std::vector<Walked>& out) {
  if (depth > 24 || out.size() >= 2000) return;
  out.push_back({describe(el), el});
  ComPtr<IUIAutomationElement> child;
  if (FAILED(tw->GetFirstChildElement(el, &child))) return;
  for (int i = 0; child.Get() && i < 500 && out.size() < 2000; ++i) {
    walkEl(tw, child.Get(), depth + 1, out);
    ComPtr<IUIAutomationElement> next;
    if (FAILED(tw->GetNextSiblingElement(child.Get(), &next))) break;
    child = next;
  }
}

std::vector<Walked> collect(void* hv) {
  ensureCom();
  std::vector<Walked> out;
  ComPtr<IUIAutomation> ua;
  if (FAILED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&ua))))
    return out;
  ComPtr<IUIAutomationElement> root;
  ComPtr<IUIAutomationTreeWalker> tw;
  if (FAILED(ua->ElementFromHandle((HWND)hv, &root)) ||
      FAILED(ua->get_ControlViewWalker(&tw)))
    return out;
  walkEl(tw.Get(), root.Get(), 0, out);
  std::map<std::string, int> seen;
  for (auto& w : out) {
    std::string base = w.node.role + ":" + w.node.name;
    int k = seen[base]++;
    w.node.id = k ? base + "#" + std::to_string(k) : base;
  }
  return out;
}

void mouse(DWORD flags) {
  INPUT in{};
  in.type = INPUT_MOUSE;
  in.mi.dwFlags = flags;
  SendInput(1, &in, sizeof in);
}

bool keyUnit(wchar_t c, DWORD flags) {
  INPUT in{};
  in.type = INPUT_KEYBOARD;
  in.ki.wScan = (WORD)c;
  in.ki.dwFlags = KEYEVENTF_UNICODE | flags;
  return SendInput(1, &in, sizeof in) == 1;
}
}  // namespace

PlugprobeOsCaps plugprobeOsCaps() { return {true, true, true, true}; }
bool plugprobeInputGranted() { return true; }

bool plugprobeGrabFrame(void* hv, PlugprobeRgbFrame& out) {
  HWND hwnd = (HWND)hv;
  RECT r{};
  if (!GetWindowRect(hwnd, &r)) return false;
  int W = r.right - r.left, H = r.bottom - r.top;
  if (W <= 0 || H <= 0) return false;
  HDC screen = GetDC(nullptr);
  if (!screen) return false;
  HDC mem = CreateCompatibleDC(screen);
  if (!mem) {
    ReleaseDC(nullptr, screen);
    return false;
  }
  BITMAPINFO bmi{};
  bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bmi.bmiHeader.biWidth = W;
  bmi.bmiHeader.biHeight = -H;  // top-down
  bmi.bmiHeader.biPlanes = 1;
  bmi.bmiHeader.biBitCount = 32;
  bmi.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  HBITMAP dib = CreateDIBSection(screen, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
  bool ok = false;
  if (dib) {
    HGDIOBJ old = SelectObject(mem, dib);
    ok = BitBlt(mem, 0, 0, W, H, screen, r.left, r.top, SRCCOPY) != FALSE;
    SelectObject(mem, old);
    if (ok) {
      auto* px = static_cast<unsigned char*>(bits);  // BGRX
      out.w = W;
      out.h = H;
      out.rgb.resize((size_t)W * H * 3);
      for (size_t i = 0; i < (size_t)W * H; ++i) {
        out.rgb[3 * i] = px[4 * i + 2];
        out.rgb[3 * i + 1] = px[4 * i + 1];
        out.rgb[3 * i + 2] = px[4 * i];
      }
    }
    DeleteObject(dib);
  }
  DeleteDC(mem);
  ReleaseDC(nullptr, screen);
  return ok;
}

int plugprobeSaveNSViewShot(void* hv, const char* path, int* w, int* h) {
  return pp::saveShotPng(hv, path, w, h);
}
int plugprobeSaveWindowShot(void* hv, const char* path, int* w, int* h) {
  return pp::saveShotPng(hv, path, w, h);
}

bool plugprobeShowFront(void* hv) {
  HWND h = (HWND)hv;
  ShowWindow(h, SW_SHOWNORMAL);
  SetForegroundWindow(h);
  return IsWindowVisible(h) != FALSE;
}

std::vector<PlugprobeAxNode> plugprobeAxDump(void* hv) {
  std::vector<PlugprobeAxNode> nodes;
  for (auto& w : collect(hv)) nodes.push_back(w.node);
  return nodes;
}

int plugprobeAxPressById(const char* nodeId, void* hv, double* cx, double* cy) {
  for (auto& w : collect(hv)) {
    if (w.node.id != nodeId) continue;
    if (cx) *cx = w.node.x + w.node.w / 2;
    if (cy) *cy = w.node.y + w.node.h / 2;
    ComPtr<IUIAutomationInvokePattern> ip;
    if (FAILED(w.el->GetCurrentPatternAs(UIA_InvokePatternId, IID_PPV_ARGS(&ip))))
      return -1;
    return SUCCEEDED(ip->Invoke()) ? 1 : -1;
  }
  return 0;
}

int plugprobeAxSetValueById(const char* nodeId, void* hv, double value,
                            double* actual) {
  for (auto& w : collect(hv)) {
    if (w.node.id != nodeId) continue;
    ComPtr<IUIAutomationRangeValuePattern> rp;
    if (FAILED(w.el->GetCurrentPatternAs(UIA_RangeValuePatternId,
                                         IID_PPV_ARGS(&rp))))
      return -1;
    if (FAILED(rp->SetValue(value))) return -1;
    double v = 0;
    rp->get_CurrentValue(&v);
    if (actual) *actual = v;
    return 1;
  }
  return 0;
}

bool plugprobeAxClickAt(double x, double y) {
  if (!SetCursorPos((int)x, (int)y)) return false;
  mouse(MOUSEEVENTF_LEFTDOWN);
  Sleep(60);
  mouse(MOUSEEVENTF_LEFTUP);
  plugprobePumpApp(0.4);
  return true;
}

bool plugprobeAxDragAt(double x, double y, double dx, double dy) {
  if (!SetCursorPos((int)x, (int)y)) return false;
  mouse(MOUSEEVENTF_LEFTDOWN);
  for (int i = 1; i <= 8; ++i) {
    SetCursorPos((int)(x + dx * i / 8), (int)(y + dy * i / 8));
    Sleep(8);
  }
  mouse(MOUSEEVENTF_LEFTUP);
  plugprobePumpApp(0.4);
  return true;
}

bool plugprobeAxTypeText(const char* text) {
  for (wchar_t c : toWide(text)) {
    if (!keyUnit(c, 0) || !keyUnit(c, KEYEVENTF_KEYUP)) return false;
  }
  plugprobePumpApp(0.3);
  return true;
}

void plugprobeFocusWindow(void* hv) {
  HWND h = (HWND)hv;
  DWORD fg = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
  DWORD me = GetCurrentThreadId();
  bool attach = fg != 0 && fg != me;
  if (attach) AttachThreadInput(fg, me, TRUE);  // lets us take focus from another process
  SetForegroundWindow(h);
  BringWindowToTop(h);
  if (attach) AttachThreadInput(fg, me, FALSE);
  plugprobePumpApp(0.3);
}

void plugprobePumpApp(double seconds) {
  ULONGLONG end = GetTickCount64() + (ULONGLONG)(seconds * 1000.0);
  MSG m;
  for (;;) {
    while (PeekMessage(&m, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&m);
      DispatchMessage(&m);
    }
    if (GetTickCount64() >= end) return;
    Sleep(5);
  }
}

void plugprobeTryActivate() {}
