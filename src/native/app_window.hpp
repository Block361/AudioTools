#pragma once
#include "aoip/version.hpp"
#include "ui.hpp"
namespace aoip::ui {
class AppWindow {
public:
  HWND window = nullptr, owner = nullptr;
  std::map<int, HWND> controls;
  bool preview = false, modal = false;
  int width = 1000, height = 700;
  double scale = 1;
  std::wstring message = L"Ready";
  COLORREF message_color = muted;
  virtual ~AppWindow() = default;
  int run(HINSTANCE, const wchar_t* class_name, const std::wstring& title, int w, int h, int show,
          const std::filesystem::path& preview_file = {});
  void refresh();
  HWND add(int id, const wchar_t* type, const std::wstring& label, DWORD style = 0);
  HWND add_button(int id, const std::wstring& label);
  HWND add_combo(int id, const std::vector<std::wstring>& labels, int selected = 0);
  int selection(int id) const;
  void select(int id, int index);
  void position(int id, Box);
  void header(HDC, const std::wstring& title, const std::wstring& state = L"",
              COLORREF color = gray, bool application_icon = false);
  virtual void created() {};
  virtual void layout() {};
  virtual void timer() {};
  virtual void command(int, int) {};
  virtual void paint(HDC) = 0;
  virtual void click(int, int, bool) {};
  virtual void scroll(int, int) {};
  virtual bool close() {
    return true;
  }

private:
  static LRESULT CALLBACK procedure(HWND, UINT, WPARAM, LPARAM);
  void draw(HDC);
};
} // namespace aoip::ui
