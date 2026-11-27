#pragma once
#include "aoip/system.hpp"
#include "app_window.hpp"
namespace aoip::ui {
void manage_iot(HWND owner, const Json& devices, const std::string& selected = "");
}
