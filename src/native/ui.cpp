#include "ui.hpp"
#include <algorithm>
#include <cmath>
#include <commdlg.h>
#include <dwmapi.h>
#include <map>
#include <shellapi.h>
#include <shlobj.h>
#include <stdexcept>
#include <uxtheme.h>
#include <wincodec.h>
namespace aoip::ui {
std::wstring wide(const std::string& s) {
  if (s.empty())
    return {};
  const auto n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring r(n, 0);
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), r.data(), n);
  return r;
}
std::string utf8(const std::wstring& s) {
  if (s.empty())
    return {};
  const auto n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0,
                                     nullptr, nullptr);
  std::string r(n, 0);
  WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), r.data(), n, nullptr,
                      nullptr);
  return r;
}
HFONT font(int size, int weight) {
  static std::map<std::pair<int, int>, HFONT> cache;
  const auto key = std::pair{size, weight};
  auto& f = cache[key];
  if (!f)
    f = CreateFontW(-size, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                    OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                    L"Segoe UI");
  return f;
}
void fill(HDC dc, Box b, COLORREF color) {
  const auto brush = CreateSolidBrush(color);
  RECT r{b.x, b.y, b.x + b.w, b.y + b.h};
  FillRect(dc, &r, brush);
  DeleteObject(brush);
}
void rounded(HDC dc, Box b, COLORREF color, int radius, COLORREF outline) {
  const auto brush = CreateSolidBrush(color);
  const auto old = SelectObject(dc, brush);
  const auto pen = CreatePen(PS_SOLID, 1, outline);
  const auto oldpen = SelectObject(dc, pen);
  RoundRect(dc, b.x, b.y, b.x + b.w, b.y + b.h, radius * 2, radius * 2);
  SelectObject(dc, oldpen);
  SelectObject(dc, old);
  DeleteObject(pen);
  DeleteObject(brush);
}
void text(HDC dc, Box b, const std::wstring& s, int size, COLORREF color, int weight, UINT align) {
  const auto old = SelectObject(dc, font(size, weight));
  SetBkMode(dc, TRANSPARENT);
  SetTextColor(dc, color);
  RECT r{b.x, b.y, b.x + b.w, b.y + b.h};
  DrawTextW(dc, s.c_str(), static_cast<int>(s.size()), &r, align | DT_END_ELLIPSIS | DT_NOPREFIX);
  SelectObject(dc, old);
}
void line(HDC dc, int x, int y, int xx, int yy, COLORREF color, int width) {
  const auto p = CreatePen(PS_SOLID, width, color);
  const auto old = SelectObject(dc, p);
  MoveToEx(dc, x, y, nullptr);
  LineTo(dc, xx, yy);
  SelectObject(dc, old);
  DeleteObject(p);
}
void dot(HDC dc, int x, int y, COLORREF color, int radius) {
  const auto b = CreateSolidBrush(color);
  const auto old = SelectObject(dc, b);
  const auto p = SelectObject(dc, GetStockObject(NULL_PEN));
  Ellipse(dc, x - radius, y - radius, x + radius + 1, y + radius + 1);
  SelectObject(dc, p);
  SelectObject(dc, old);
  DeleteObject(b);
}
void logo(HDC dc, int x, int y, int size) {
  rounded(dc, {x, y, size, size}, RGB(36, 66, 76), 9, RGB(36, 66, 76));
  for (int row = 0; row < 3; ++row) {
    POINT points[30];
    for (int i = 0; i < 30; ++i) {
      points[i] = {x + size / 5 + i * size / 50,
                   y + size / 3 + row * size / 6 +
                       static_cast<int>(std::sin(i * .24) * size * .07)};
    }
    const auto pen = CreatePen(PS_SOLID, 2, mint);
    const auto old = SelectObject(dc, pen);
    Polyline(dc, points, 30);
    SelectObject(dc, old);
    DeleteObject(pen);
  }
}
void rotated(HDC dc, Box b, const std::wstring& s, int size, COLORREF color) {
  const auto f = CreateFontW(-size, 0, 900, 900, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH, L"Segoe UI");
  const auto old = SelectObject(dc, f);
  SetTextColor(dc, color);
  SetBkMode(dc, TRANSPARENT);
  TextOutW(dc, b.x + 5, b.y + b.h - 5, s.c_str(),
           static_cast<int>(std::min<std::size_t>(s.size(), 23)));
  SelectObject(dc, old);
  DeleteObject(f);
}
void button(HDC dc, Box b, const std::wstring& caption, bool primary, bool enabled) {
  rounded(dc, b, primary ? teal : surface, 3, primary ? teal : border);
  text(dc, b, caption, 12,
       !enabled  ? RGB(170, 180, 185)
       : primary ? white
                 : ink,
       FW_SEMIBOLD, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}
HWND control(HWND parent, const wchar_t* type, const std::wstring& caption, DWORD style, int id) {
  const auto result = CreateWindowExW(
      0, type, caption.c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | style, 0, 0, 10, 10, parent,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
  SendMessageW(result, WM_SETFONT, reinterpret_cast<WPARAM>(font()), TRUE);
  SetWindowTheme(result, L"DarkMode_CFD", nullptr);
  return result;
}
void place(HWND w, Box b, double scale) {
  SendMessageW(w, WM_SETFONT, reinterpret_cast<WPARAM>(font(static_cast<int>(14 * scale))), TRUE);
  MoveWindow(w, static_cast<int>(b.x * scale), static_cast<int>(b.y * scale),
             static_cast<int>(b.w * scale), static_cast<int>(b.h * scale), TRUE);
}
void set_text(HWND h, const std::wstring& s) {
  SetWindowTextW(h, s.c_str());
}
std::wstring get_text(HWND h) {
  const int n = GetWindowTextLengthW(h);
  std::wstring s(n + 1, 0);
  GetWindowTextW(h, s.data(), n + 1);
  s.resize(n);
  return s;
}
std::filesystem::path data_directory(const std::wstring& app) {
  PWSTR raw = nullptr;
  if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &raw)))
    throw std::runtime_error("Cannot locate application settings folder");
  const auto p = std::filesystem::path(raw) / L"OpenAoIP" / app;
  CoTaskMemFree(raw);
  std::filesystem::create_directories(p);
  return p;
}
std::filesystem::path executable_directory() {
  wchar_t path[32768]{};
  GetModuleFileNameW(nullptr, path, 32768);
  return std::filesystem::path(path).parent_path();
}
std::vector<std::wstring> arguments() {
  int count = 0;
  auto** argv = CommandLineToArgvW(GetCommandLineW(), &count);
  std::vector<std::wstring> values;
  for (int i = 1; i < count; ++i)
    values.emplace_back(argv[i]);
  LocalFree(argv);
  return values;
}
void enable_dpi() {
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
}
void dark_window(HWND window) {
  BOOL value = TRUE;
  DwmSetWindowAttribute(window, 20, &value, sizeof(value));
}
HBRUSH control_brush() {
  static HBRUSH brush = CreateSolidBrush(surface);
  return brush;
}
void meter(HDC dc, Box box, float value) {
  fill(dc, box, surface);
  const float db = value > 0 ? 20.f * std::log10(value) : -80.f;
  const int lit = static_cast<int>(std::clamp((db + 60.f) / 60.f, 0.f, 1.f) * 24.f);
  const int gap = 2, cell = std::max(2, (box.w - 23 * gap) / 24);
  for (int i = 0; i < 24; ++i)
    fill(dc, {box.x + i * (cell + gap), box.y, cell, box.h},
         i < lit ? (i >= 23   ? red
                    : i >= 19 ? amber
                              : green)
                 : border);
}
namespace {
struct Prompt {
  HWND edit = nullptr;
  std::wstring value;
  bool accepted = false;
  int limit = 80;
};
LRESULT CALLBACK prompt_proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
  auto* s = reinterpret_cast<Prompt*>(GetWindowLongPtrW(h, GWLP_USERDATA));
  if (m == WM_NCCREATE) {
    s = static_cast<Prompt*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
    SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(s));
  }
  if (!s)
    return DefWindowProcW(h, m, wp, lp);
  const auto scale = GetDpiForWindow(h) / 96.;
  if (m == WM_CREATE) {
    s->edit = control(h, L"EDIT", s->value, WS_BORDER | ES_AUTOHSCROLL, 3);
    SendMessageW(s->edit, EM_SETLIMITTEXT, s->limit, 0);
    place(s->edit, {16, 18, 400, 28}, scale);
    auto save = control(h, L"BUTTON", L"Save", BS_OWNERDRAW, IDOK),
         cancel = control(h, L"BUTTON", L"Cancel", BS_OWNERDRAW, IDCANCEL);
    place(save, {224, 62, 92, 28}, scale);
    place(cancel, {324, 62, 92, 28}, scale);
    return 0;
  }
  if (m == WM_COMMAND) {
    if (LOWORD(wp) == IDOK) {
      s->value = get_text(s->edit);
      s->accepted = true;
      DestroyWindow(h);
    } else if (LOWORD(wp) == IDCANCEL)
      DestroyWindow(h);
    return 0;
  }
  if (m == WM_DRAWITEM) {
    auto* d = reinterpret_cast<DRAWITEMSTRUCT*>(lp);
    const int saved = SaveDC(d->hDC);
    SetMapMode(d->hDC, MM_ANISOTROPIC);
    SetWindowExtEx(d->hDC, 92, 28, nullptr);
    SetViewportExtEx(d->hDC, d->rcItem.right, d->rcItem.bottom, nullptr);
    button(d->hDC, {0, 0, 92, 28}, get_text(d->hwndItem), d->CtlID == IDOK);
    RestoreDC(d->hDC, saved);
    return TRUE;
  }
  if (m == WM_CTLCOLOREDIT || m == WM_CTLCOLORSTATIC) {
    SetTextColor(reinterpret_cast<HDC>(wp), ink);
    SetBkColor(reinterpret_cast<HDC>(wp), surface);
    return reinterpret_cast<LRESULT>(control_brush());
  }
  if (m == WM_ERASEBKGND) {
    RECT r{};
    GetClientRect(h, &r);
    FillRect(reinterpret_cast<HDC>(wp), &r, control_brush());
    return 1;
  }
  return DefWindowProcW(h, m, wp, lp);
}
} // namespace
std::optional<std::wstring> prompt(HWND parent, const std::wstring& title,
                                   const std::wstring& value, int limit) {
  static bool registered = false;
  if (!registered) {
    WNDCLASSW cls{};
    cls.hInstance = GetModuleHandleW(nullptr);
    cls.lpfnWndProc = prompt_proc;
    cls.lpszClassName = L"OpenAoIP.TextPrompt";
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&cls);
    registered = true;
  }
  Prompt data;
  data.value = value;
  data.limit = limit;
  const auto scale = GetDpiForWindow(parent) / 96.;
  RECT p{};
  GetWindowRect(parent, &p);
  HWND h = CreateWindowExW(WS_EX_DLGMODALFRAME, L"OpenAoIP.TextPrompt", title.c_str(),
                           WS_CAPTION | WS_SYSMENU, p.left + 70, p.top + 90,
                           static_cast<int>(450 * scale), static_cast<int>(140 * scale), parent,
                           nullptr, GetModuleHandleW(nullptr), &data);
  if (!h)
    return std::nullopt;
  dark_window(h);
  EnableWindow(parent, FALSE);
  ShowWindow(h, SW_SHOW);
  SetFocus(data.edit);
  SendMessageW(data.edit, EM_SETSEL, 0, -1);
  MSG msg{};
  while (IsWindow(h) && GetMessageW(&msg, nullptr, 0, 0) > 0) {
    if (!IsDialogMessageW(h, &msg)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
  }
  EnableWindow(parent, TRUE);
  SetForegroundWindow(parent);
  return data.accepted ? std::optional(data.value) : std::nullopt;
}
std::optional<std::filesystem::path> json_file_dialog(HWND owner, bool save,
                                                      const std::wstring& name) {
  wchar_t path[32768]{};
  wcsncpy_s(path, name.c_str(), _TRUNCATE);
  OPENFILENAMEW info{sizeof(info)};
  info.hwndOwner = owner;
  info.lpstrFilter = L"OpenAoIP JSON configuration\0*.json\0\0";
  info.lpstrFile = path;
  info.nMaxFile = 32768;
  info.lpstrDefExt = L"json";
  info.Flags = OFN_PATHMUSTEXIST | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
  return (save ? GetSaveFileNameW(&info) : GetOpenFileNameW(&info))
             ? std::optional<std::filesystem::path>(path)
             : std::nullopt;
}
template <class T> struct Com {
  T* p = nullptr;
  ~Com() {
    if (p)
      p->Release();
  }
  T** out() {
    return &p;
  }
  T* operator->() {
    return p;
  }
};
void save_png(const std::filesystem::path& path, int width, int height,
              void (*paint)(HDC, int, int, void*), void* context) {
  const auto dc = CreateCompatibleDC(nullptr);
  BITMAPINFO info{};
  info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  info.bmiHeader.biWidth = width;
  info.bmiHeader.biHeight = -height;
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  info.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  const auto bmp = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (!bmp)
    throw std::runtime_error("Cannot render native preview");
  const auto old = SelectObject(dc, bmp);
  paint(dc, width, height, context);
  std::filesystem::create_directories(std::filesystem::absolute(path).parent_path());
  Com<IWICImagingFactory> factory;
  Com<IWICBitmap> bitmap;
  Com<IWICStream> stream;
  Com<IWICBitmapEncoder> encoder;
  Com<IWICBitmapFrameEncode> frame;
  auto check = [](HRESULT r) {
    if (FAILED(r))
      throw std::runtime_error("Native preview PNG encoding failed");
  };
  check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                         IID_PPV_ARGS(factory.out())));
  check(factory->CreateBitmapFromHBITMAP(bmp, nullptr, WICBitmapIgnoreAlpha, bitmap.out()));
  check(factory->CreateStream(stream.out()));
  check(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE));
  check(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.out()));
  check(encoder->Initialize(stream.p, WICBitmapEncoderNoCache));
  check(encoder->CreateNewFrame(frame.out(), nullptr));
  check(frame->Initialize(nullptr));
  check(frame->SetSize(width, height));
  auto format = GUID_WICPixelFormat32bppBGR;
  check(frame->SetPixelFormat(&format));
  check(frame->WriteSource(bitmap.p, nullptr));
  check(frame->Commit());
  check(encoder->Commit());
  SelectObject(dc, old);
  DeleteObject(bmp);
  DeleteDC(dc);
}
} // namespace aoip::ui
