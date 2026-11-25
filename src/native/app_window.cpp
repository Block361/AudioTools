#include "app_window.hpp"
namespace aoip::ui {
HWND AppWindow::add(int id, const wchar_t* type, const std::wstring& label, DWORD style) {
  auto child = control(window, type, label, style, id);
  controls[id] = child;
  return child;
}
HWND AppWindow::add_button(int id, const std::wstring& label) {
  return add(id, L"BUTTON", label, BS_OWNERDRAW);
}
HWND AppWindow::add_combo(int id, const std::vector<std::wstring>& labels, int value) {
  auto combo = add(id, L"COMBOBOX", L"",
                   CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_VSCROLL);
  for (const auto& label : labels)
    SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
  select(id, value);
  return combo;
}
int AppWindow::selection(int id) const {
  return static_cast<int>(SendMessageW(controls.at(id), CB_GETCURSEL, 0, 0));
}
void AppWindow::select(int id, int index) {
  SendMessageW(controls.at(id), CB_SETCURSEL, index, 0);
}
void AppWindow::position(int id, Box box) {
  place(controls.at(id), box, scale);
}
void AppWindow::refresh() {
  if (window)
    InvalidateRect(window, nullptr, FALSE);
}
void AppWindow::header(HDC dc, const std::wstring& title, const std::wstring& state, COLORREF color,
                       bool application_icon) {
  fill(dc, {0, 0, width, 54}, navy);
  // Use the executable's own artwork so the header and Windows shell stay consistent.
  const auto icon =
      application_icon
          ? static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1),
                                          IMAGE_ICON, 32, 32, LR_SHARED))
          : nullptr;
  if (icon)
    DrawIconEx(dc, 14, 10, icon, 32, 32, 0, nullptr, DI_NORMAL);
  else
    logo(dc, 14, 10, 32);
  text(dc, {58, 8, width - 300, 25}, title, 19, ink, FW_SEMIBOLD);
  text(dc, {59, 32, width - 330, 14},
       L"OPENAOIP  " + wide(aoip::version) + (preview ? L"  /  PREVIEW" : L""), 9, muted);
  if (!state.empty()) {
    dot(dc, width - 175, 27, color);
    text(dc, {width - 162, 12, 150, 28}, state, 12, color);
  }
  line(dc, 0, 53, width, 53, border);
}
void AppWindow::draw(HDC dc) {
  fill(dc, {0, 0, width, height}, background);
  paint(dc);
  line(dc, 0, height - 27, width, height - 27, border);
  dot(dc, 12, height - 13, message_color, 3);
  text(dc, {23, height - 26, width - 35, 25}, message, 11, muted);
}
LRESULT CALLBACK AppWindow::procedure(HWND h, UINT m, WPARAM wp, LPARAM lp) {
  auto* self = reinterpret_cast<AppWindow*>(GetWindowLongPtrW(h, GWLP_USERDATA));
  if (m == WM_NCCREATE) {
    self = static_cast<AppWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
    self->window = h;
    SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
  }
  if (!self)
    return DefWindowProcW(h, m, wp, lp);
  self->scale = GetDpiForWindow(h) / 96.;
  RECT bounds{};
  GetClientRect(h, &bounds);
  self->width = static_cast<int>(bounds.right / self->scale);
  self->height = static_cast<int>(bounds.bottom / self->scale);
  switch (m) {
  case WM_CREATE:
    dark_window(h);
    try {
      self->created();
      self->layout();
      SetTimer(h, 1, 250, nullptr);
      return 0;
    } catch (const std::exception& e) {
      self->message = wide(e.what());
      return -1;
    }
  case WM_TIMER:
    try {
      self->timer();
    } catch (const std::exception& e) {
      self->message = wide(e.what());
      self->message_color = red;
    }
    self->refresh();
    return 0;
  case WM_ERASEBKGND:
    return 1;
  case WM_PRINTCLIENT: {
    const int saved = SaveDC(reinterpret_cast<HDC>(wp));
    SetMapMode(reinterpret_cast<HDC>(wp), MM_ANISOTROPIC);
    SetWindowExtEx(reinterpret_cast<HDC>(wp), self->width, self->height, nullptr);
    SetViewportExtEx(reinterpret_cast<HDC>(wp), bounds.right, bounds.bottom, nullptr);
    self->draw(reinterpret_cast<HDC>(wp));
    RestoreDC(reinterpret_cast<HDC>(wp), saved);
    return 0;
  }
  case WM_PAINT: {
    PAINTSTRUCT ps{};
    const auto dc = BeginPaint(h, &ps);
    auto memory = CreateCompatibleDC(dc);
    auto bitmap = CreateCompatibleBitmap(dc, std::max<LONG>(1, bounds.right),
                                         std::max<LONG>(1, bounds.bottom));
    auto old = SelectObject(memory, bitmap);
    const int saved = SaveDC(memory);
    SetMapMode(memory, MM_ANISOTROPIC);
    SetWindowExtEx(memory, std::max(1, self->width), std::max(1, self->height), nullptr);
    SetViewportExtEx(memory, std::max<LONG>(1, bounds.right), std::max<LONG>(1, bounds.bottom),
                     nullptr);
    self->draw(memory);
    RestoreDC(memory, saved);
    BitBlt(dc, 0, 0, bounds.right, bounds.bottom, memory, 0, 0, SRCCOPY);
    SelectObject(memory, old);
    DeleteObject(bitmap);
    DeleteDC(memory);
    EndPaint(h, &ps);
    return 0;
  }
  case WM_MEASUREITEM: {
    auto* item = reinterpret_cast<MEASUREITEMSTRUCT*>(lp);
    if (item->CtlType == ODT_COMBOBOX) {
      item->itemHeight = static_cast<UINT>(23 * self->scale);
      return TRUE;
    }
    break;
  }
  case WM_DRAWITEM: {
    auto* item = reinterpret_cast<DRAWITEMSTRUCT*>(lp);
    if (item->CtlType == ODT_COMBOBOX) {
      const int saved = SaveDC(item->hDC);
      SetMapMode(item->hDC, MM_TEXT);
      const auto& r = item->rcItem;
      const bool enabled = IsWindowEnabled(item->hwndItem);
      const bool chosen = (item->itemState & ODS_SELECTED) && !(item->itemState & ODS_COMBOBOXEDIT);
      fill(item->hDC, {r.left, r.top, r.right - r.left, r.bottom - r.top},
           chosen ? selected : surface);
      const int index = item->itemID == static_cast<UINT>(-1)
                            ? static_cast<int>(SendMessageW(item->hwndItem, CB_GETCURSEL, 0, 0))
                            : static_cast<int>(item->itemID);
      if (index >= 0) {
        const auto length = SendMessageW(item->hwndItem, CB_GETLBTEXTLEN, index, 0);
        if (length >= 0 && length < 32768) {
          std::wstring label(static_cast<std::size_t>(length) + 1, 0);
          SendMessageW(item->hwndItem, CB_GETLBTEXT, index, reinterpret_cast<LPARAM>(label.data()));
          label.resize(static_cast<std::size_t>(length));
          const int pad = static_cast<int>(6 * self->scale);
          text(
              item->hDC,
              {r.left + pad, r.top, std::max(1, int(r.right - r.left) - pad * 2), r.bottom - r.top},
              label, static_cast<int>(12 * self->scale), enabled ? ink : gray);
        }
      }
      RestoreDC(item->hDC, saved);
      return TRUE;
    }
    const int saved = SaveDC(item->hDC);
    const auto w = static_cast<int>((item->rcItem.right - item->rcItem.left) / self->scale),
               v = static_cast<int>((item->rcItem.bottom - item->rcItem.top) / self->scale);
    SetMapMode(item->hDC, MM_ANISOTROPIC);
    SetWindowExtEx(item->hDC, std::max(1, w), std::max(1, v), nullptr);
    SetViewportExtEx(item->hDC, item->rcItem.right, item->rcItem.bottom, nullptr);
    button(item->hDC, {0, 0, w, v}, get_text(item->hwndItem), item->CtlID == 109,
           IsWindowEnabled(item->hwndItem));
    RestoreDC(item->hDC, saved);
    return TRUE;
  }
  case WM_COMMAND:
    try {
      self->command(LOWORD(wp), HIWORD(wp));
    } catch (const std::exception& e) {
      self->message = wide(e.what());
      self->message_color = red;
    }
    self->refresh();
    return 0;
  case WM_LBUTTONDOWN:
  case WM_RBUTTONUP:
  case WM_LBUTTONDBLCLK:
    try {
      self->click(static_cast<int>(static_cast<short>(LOWORD(lp)) / self->scale),
                  static_cast<int>(static_cast<short>(HIWORD(lp)) / self->scale),
                  m != WM_LBUTTONDOWN);
    } catch (const std::exception& e) {
      self->message = wide(e.what());
      self->message_color = red;
    }
    self->refresh();
    return 0;
  case WM_HSCROLL:
  case WM_VSCROLL:
    self->scroll(m == WM_HSCROLL ? SB_HORZ : SB_VERT,
                 LOWORD(wp) == SB_THUMBTRACK ? HIWORD(wp) : -1000 - LOWORD(wp));
    self->refresh();
    return 0;
  case WM_MOUSEWHEEL:
    self->scroll(SB_VERT, GET_WHEEL_DELTA_WPARAM(wp) > 0 ? -1000 - SB_LINEUP : -1000 - SB_LINEDOWN);
    self->refresh();
    return 0;
  case WM_CTLCOLOREDIT:
  case WM_CTLCOLORLISTBOX:
  case WM_CTLCOLORSTATIC:
    SetTextColor(reinterpret_cast<HDC>(wp), ink);
    SetBkColor(reinterpret_cast<HDC>(wp), surface);
    return reinterpret_cast<LRESULT>(control_brush());
  case WM_SIZE:
    self->layout();
    self->refresh();
    return 0;
  case WM_DPICHANGED: {
    auto* r = reinterpret_cast<RECT*>(lp);
    SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER);
    self->layout();
    return 0;
  }
  case WM_GETMINMAXINFO: {
    auto* info = reinterpret_cast<MINMAXINFO*>(lp);
    info->ptMinTrackSize = {static_cast<LONG>(720 * self->scale),
                            static_cast<LONG>(480 * self->scale)};
    return 0;
  }
  case WM_CLOSE:
    if (self->close())
      DestroyWindow(h);
    return 0;
  case WM_DESTROY:
    KillTimer(h, 1);
    if (!self->preview && !self->modal)
      PostQuitMessage(0);
    return 0;
  }
  return DefWindowProcW(h, m, wp, lp);
}
int AppWindow::run(HINSTANCE instance, const wchar_t* class_name, const std::wstring& title, int w,
                   int h, int show, const std::filesystem::path& preview_file) {
  preview = !preview_file.empty();
  DPI_AWARENESS_CONTEXT old = nullptr;
  if (preview)
    old = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_UNAWARE);
  WNDCLASSEXW cls{sizeof(cls)};
  cls.hInstance = instance;
  cls.lpfnWndProc = procedure;
  cls.lpszClassName = class_name;
  cls.style = CS_DBLCLKS;
  cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  cls.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
  cls.hIconSm = cls.hIcon;
  RegisterClassExW(&cls);
  const auto dpi = preview ? 96 : GetDpiForSystem();
  RECT r{0, 0, MulDiv(w, dpi, 96), MulDiv(h, dpi, 96)};
  AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
  RECT work{};
  SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
  const auto hwnd = CreateWindowExW(
      0, class_name, title.c_str(), WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT,
      CW_USEDEFAULT,
      preview ? r.right - r.left : std::min<LONG>(r.right - r.left, work.right - work.left),
      preview ? r.bottom - r.top : std::min<LONG>(r.bottom - r.top, work.bottom - work.top), owner,
      nullptr, instance, this);
  if (!hwnd)
    throw std::runtime_error(utf8(message));
  if (preview) {
    save_png(
        preview_file, w, h,
        [](HDC dc, int, int, void* context) {
          auto* app = static_cast<AppWindow*>(context);
          app->draw(dc);
          // WM_PRINT on the top-level window offsets child painting by its caption/frame.
          // Print each real child at its client-relative position instead.
          for (const auto& [id, child] : app->controls) {
            if (!(GetWindowLongPtrW(child, GWL_STYLE) & WS_VISIBLE))
              continue;
            RECT bounds{};
            GetWindowRect(child, &bounds);
            MapWindowPoints(nullptr, app->window, reinterpret_cast<POINT*>(&bounds), 2);
            const int saved = SaveDC(dc);
            SetViewportOrgEx(dc, bounds.left, bounds.top, nullptr);
            SendMessageW(child, WM_PRINT, reinterpret_cast<WPARAM>(dc),
                         PRF_CLIENT | PRF_NONCLIENT | PRF_CHILDREN | PRF_ERASEBKGND);
            wchar_t type[32]{};
            GetClassNameW(child, type, 32);
            if (_wcsicmp(type, L"COMBOBOX") == 0) {
              // Hidden themed combos omit their selection field in WM_PRINT.
              DRAWITEMSTRUCT item{};
              item.CtlType = ODT_COMBOBOX;
              item.CtlID = id;
              item.itemID = static_cast<UINT>(SendMessageW(child, CB_GETCURSEL, 0, 0));
              item.itemState = ODS_COMBOBOXEDIT;
              item.hwndItem = child;
              item.hDC = dc;
              item.rcItem = {3, 3, std::max(4L, bounds.right - bounds.left - 25),
                             std::max(4L, bounds.bottom - bounds.top - 3)};
              SendMessageW(app->window, WM_DRAWITEM, id, reinterpret_cast<LPARAM>(&item));
            }
            RestoreDC(dc, saved);
          }
        },
        this);
    DestroyWindow(hwnd);
    if (old)
      SetThreadDpiAwarenessContext(old);
    return 0;
  }
  if (modal && owner)
    EnableWindow(owner, FALSE);
  ShowWindow(hwnd, show);
  UpdateWindow(hwnd);
  MSG msg{};
  while (IsWindow(hwnd) && GetMessageW(&msg, nullptr, 0, 0) > 0) {
    if (!IsDialogMessageW(hwnd, &msg)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
  }
  if (modal && owner) {
    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);
  }
  return 0;
}
} // namespace aoip::ui
