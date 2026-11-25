#pragma once
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <map>
#include <objbase.h>
#include <string>
#include <vector>
#include <optional>
#include <windows.h>
namespace aoip::ui {
struct Apartment {
  HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  ~Apartment() {
    if (SUCCEEDED(result))
      CoUninitialize();
  }
};
inline constexpr COLORREF navy = RGB(17, 24, 32), ink = RGB(232, 239, 245), muted = RGB(145, 162, 177),
                          teal = RGB(24, 150, 143), mint = RGB(166, 240, 223),
                          background = RGB(20, 27, 35),surface=RGB(27,35,44),selected=RGB(24,70,80),border = RGB(49, 62, 75),
                          white = RGB(255, 255, 255), amber = RGB(191, 139, 40),
                          red = RGB(231, 88, 78),green=RGB(36,193,104),gray=RGB(100,115,129);
struct Box {
  int x, y, w, h;
  bool contains(int px, int py) const {
    return px >= x && py >= y && px < x + w && py < y + h;
  }
};
std::wstring wide(const std::string&);
std::string utf8(const std::wstring&);
void fill(HDC, Box, COLORREF);
void rounded(HDC, Box, COLORREF, int radius = 3, COLORREF outline = border);
void text(HDC, Box, const std::wstring&, int size = 14, COLORREF color = ink,
          int weight = FW_NORMAL, UINT align = DT_LEFT | DT_VCENTER | DT_SINGLELINE);
void line(HDC, int, int, int, int, COLORREF, int width = 1);
void dot(HDC, int, int, COLORREF, int radius = 4);
void logo(HDC, int, int, int size = 38);
void rotated(HDC, Box, const std::wstring&, int size = 12, COLORREF color = ink);
void button(HDC, Box, const std::wstring&, bool primary = false, bool enabled = true);
HFONT font(int size = 14, int weight = FW_NORMAL);
HWND control(HWND, const wchar_t* type, const std::wstring&, DWORD style, int id);
void place(HWND, Box, double scale = 1);
void set_text(HWND, const std::wstring&);
std::wstring get_text(HWND);
std::filesystem::path data_directory(const std::wstring& app);
std::filesystem::path executable_directory();
std::vector<std::wstring> arguments();
void save_png(const std::filesystem::path&, int width, int height,
              void (*paint)(HDC, int, int, void*), void* context);
void enable_dpi();
void dark_window(HWND);HBRUSH control_brush();
std::optional<std::wstring> prompt(HWND,const std::wstring& title,const std::wstring& value,int max_length=80);
std::optional<std::filesystem::path> json_file_dialog(HWND,bool save,const std::wstring& name=L"OpenAoIP.json");
void meter(HDC,Box,float peak);
} // namespace aoip::ui
