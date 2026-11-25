// SPDX-License-Identifier: MIT
#include "aoip/version.hpp"
#include "ui.hpp"
#include <fstream>
#include <shellapi.h>
#include <shlobj.h>
#include <sstream>
#include <stdexcept>

namespace {
using namespace aoip::ui;
const std::wstring product = OPENAOIP_PRODUCT;
const std::wstring title = product == L"Controller"         ? L"OpenAoIP Controller"
                           : product == L"VirtualSoundcard" ? L"OpenAoIP Virtual Soundcard"
                           : product == L"AudioBridge"      ? L"OpenAoIP Audio Bridge"
                                                            : L"OpenAoIP Suite";
const std::wstring executable = product == L"Suite" ? L"OpenAoIP Controller.exe" : title + L".exe";
const std::wstring setup_name = L"OpenAoIP" + product + L"Setup.exe";
std::string resource(int id) {
  auto found = FindResourceW(nullptr, MAKEINTRESOURCEW(id), RT_RCDATA);
  if (!found)
    throw std::runtime_error("Installer payload resource missing");
  auto loaded = LoadResource(nullptr, found);
  auto* data = static_cast<const char*>(LockResource(loaded));
  return {data, SizeofResource(nullptr, found)};
}
std::vector<std::pair<std::filesystem::path, int>> files() {
  std::vector<std::pair<std::filesystem::path, int>> result;
  std::istringstream input(resource(100));
  std::string line;
  while (std::getline(input, line)) {
    auto split = line.find('|');
    if (split == std::string::npos)
      continue;
    auto path = std::filesystem::path(wide(line.substr(0, split)));
    if (path.is_absolute())
      throw std::runtime_error("Invalid installer path");
    for (const auto& part : path)
      if (part == L"..")
        throw std::runtime_error("Invalid installer path");
    result.emplace_back(path, std::stoi(line.substr(split + 1)));
  }
  return result;
}
std::filesystem::path known(REFKNOWNFOLDERID id) {
  PWSTR raw = nullptr;
  if (FAILED(SHGetKnownFolderPath(id, 0, nullptr, &raw)))
    throw std::runtime_error("Windows folder unavailable");
  std::filesystem::path result(raw);
  CoTaskMemFree(raw);
  return result;
}
std::filesystem::path destination() {
  return known(FOLDERID_LocalAppData) / L"OpenAoIP" / L"Apps" / product;
}
std::filesystem::path shortcut(const std::wstring& name = title) {
  return known(FOLDERID_Programs) / L"OpenAoIP" / (name + L".lnk");
}
void extract(const std::filesystem::path& folder) {
  for (const auto& [relative, id] : files()) {
    const auto path = folder / relative;
    std::filesystem::create_directories(path.parent_path());
    const auto bytes = resource(id);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!out)
      throw std::runtime_error(
          "Cannot write payload. Close the application and its audio hosts before updating.");
  }
}
void create_shortcut(const std::filesystem::path& folder, const std::wstring& name = title,
                     const std::wstring& exe = executable) {
  IShellLinkW* link = nullptr;
  if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))))
    throw std::runtime_error("Cannot create Start menu entry");
  link->SetPath((folder / exe).c_str());
  link->SetWorkingDirectory(folder.c_str());
  link->SetDescription(name.c_str());
  link->SetIconLocation((folder / exe).c_str(), 0);
  IPersistFile* persist = nullptr;
  const auto query = link->QueryInterface(IID_PPV_ARGS(&persist));
  link->Release();
  if (FAILED(query))
    throw std::runtime_error("Cannot save Start menu entry");
  const auto path = shortcut(name);
  std::filesystem::create_directories(path.parent_path());
  const auto saved = persist->Save(path.c_str(), TRUE);
  persist->Release();
  if (FAILED(saved))
    throw std::runtime_error("Cannot save Start menu entry");
}
std::wstring key_name() {
  return L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\OpenAoIP" + product;
}
void register_uninstall(const std::filesystem::path& folder) {
  HKEY key = nullptr;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, key_name().c_str(), 0, nullptr, 0, KEY_WRITE, nullptr,
                      &key, nullptr) != ERROR_SUCCESS)
    throw std::runtime_error("Cannot register application uninstaller");
  auto value = [&](const wchar_t* name, const std::wstring& s) {
    return RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(s.c_str()),
                          static_cast<DWORD>((s.size() + 1) * sizeof(wchar_t)));
  };
  value(L"DisplayName", title);
  value(L"DisplayVersion", wide(aoip::version));
  value(L"Publisher", L"OpenAoIP");
  value(L"InstallLocation", folder.wstring());
  value(L"DisplayIcon", (folder / executable).wstring());
  value(L"UninstallString", L"\"" + (folder / setup_name).wstring() + L"\" --uninstall");
  DWORD one = 1;
  RegSetValueExW(key, L"NoModify", 0, REG_DWORD, reinterpret_cast<BYTE*>(&one), sizeof(one));
  RegSetValueExW(key, L"NoRepair", 0, REG_DWORD, reinterpret_cast<BYTE*>(&one), sizeof(one));
  RegCloseKey(key);
}
void unregister_asio_if_owned(const std::filesystem::path& folder) {
  if (product != L"VirtualSoundcard" && product != L"Suite")
    return;
  constexpr auto key_path = L"CLSID\\{40E619D1-9FA6-4A49-9317-86FA9E6A3D2B}\\InprocServer32";
  wchar_t registered[32768]{};
  DWORD bytes = sizeof(registered);
  if (RegGetValueW(HKEY_CLASSES_ROOT, key_path, nullptr, RRF_RT_REG_SZ, nullptr, registered,
                   &bytes) != ERROR_SUCCESS)
    return;
  const auto dll = folder / L"OpenAoIPASIO64.dll";
  if (_wcsicmp(registered, dll.c_str()) != 0)
    return;
  wchar_t system[MAX_PATH]{};
  GetSystemDirectoryW(system, MAX_PATH);
  const auto program = std::filesystem::path(system) / L"regsvr32.exe";
  const auto args = L"/u /s \"" + dll.wstring() + L"\"";
  SHELLEXECUTEINFOW run{sizeof(run)};
  run.fMask = SEE_MASK_NOCLOSEPROCESS;
  run.lpVerb = L"runas";
  run.lpFile = program.c_str();
  run.lpParameters = args.c_str();
  run.nShow = SW_HIDE;
  if (!ShellExecuteExW(&run))
    throw std::runtime_error(
        "ASIO unregistration was cancelled. The installed files were retained.");
  WaitForSingleObject(run.hProcess, INFINITE);
  DWORD exit = 1;
  GetExitCodeProcess(run.hProcess, &exit);
  CloseHandle(run.hProcess);
  if (exit)
    throw std::runtime_error("ASIO unregistration failed; installed files were retained.");
}
void remove_installed() {
  auto folder = destination();
  if (!std::filesystem::exists(folder / L".openaoip-install"))
    throw std::runtime_error("Installation marker missing; refusing to remove unrelated files");
  unregister_asio_if_owned(folder);
  // Delete only the enumerated payload; retain settings and unrelated files.
  std::vector<std::filesystem::path> directories;
  for (const auto& [relative, id] : files()) {
    std::filesystem::remove(folder / relative);
    auto parent = (folder / relative).parent_path();
    while (parent != folder && parent != parent.root_path()) {
      directories.push_back(parent);
      parent = parent.parent_path();
    }
  }
  std::filesystem::remove(folder / setup_name);
  std::filesystem::remove(folder / L".openaoip-install");
  std::sort(directories.rbegin(), directories.rend());
  std::error_code ignored;
  for (const auto& path : directories)
    std::filesystem::remove(path, ignored);
  std::filesystem::remove(folder, ignored);
  const auto links = product == L"Suite" ? std::vector<std::wstring>{L"OpenAoIP Controller",
                                                                     L"OpenAoIP Virtual Soundcard",
                                                                     L"OpenAoIP Audio Bridge"}
                                         : std::vector<std::wstring>{title};
  for (const auto& app : links) {
    // Remove a link only if it still points to this installation.
    IShellLinkW* link = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&link)))) {
      IPersistFile* persist = nullptr;
      if (SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&persist)))) {
        if (SUCCEEDED(persist->Load(shortcut(app).c_str(), STGM_READ))) {
          wchar_t target[32768]{};
          link->GetPath(target, 32768, nullptr, SLGP_RAWPATH);
          if (_wcsicmp(target, (folder / (std::wstring(app) + L".exe")).c_str()) == 0)
            std::filesystem::remove(shortcut(app), ignored);
        }
        persist->Release();
      }
      link->Release();
    }
  }
  std::filesystem::remove(shortcut().parent_path(), ignored);
  RegDeleteTreeW(HKEY_CURRENT_USER, key_name().c_str());
}
} // namespace
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  Apartment apartment;
  enable_dpi();
  try {
    const auto args = arguments();
    // Extraction mode is used for package verification; it changes no Windows settings.
    if (args.size() == 2 && args[0] == L"--extract") {
      extract(args[1]);
      return 0;
    }
    if (args.size() == 2 && args[0] == L"--remove") {
      HANDLE parent = OpenProcess(SYNCHRONIZE, FALSE, std::stoul(args[1]));
      if (parent) {
        WaitForSingleObject(parent, 15000);
        CloseHandle(parent);
      }
      remove_installed();
      MessageBoxW(nullptr, L"Anwendung entfernt. Persönliche Einstellungen bleiben erhalten.",
                  title.c_str(), MB_OK | MB_ICONINFORMATION);
      return 0;
    }
    if (args.size() == 1 && args[0] == L"--uninstall") {
      if (MessageBoxW(
              nullptr,
              (title + L" entfernen?\nBitte Anwendung und Audio-Hosts vorher schließen.").c_str(),
              title.c_str(), MB_YESNO | MB_ICONQUESTION) != IDYES)
        return 0;
      wchar_t temp[MAX_PATH]{};
      GetTempPathW(MAX_PATH, temp);
      const auto path = std::filesystem::path(temp) /
                        (L"OpenAoIP-remove-" + std::to_wstring(GetCurrentProcessId()) + L".exe");
      if (!CopyFileW((executable_directory() / setup_name).c_str(), path.c_str(), FALSE))
        throw std::runtime_error("Cannot prepare uninstaller");
      const auto parameters = L"--remove " + std::to_wstring(GetCurrentProcessId());
      if (reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", path.c_str(),
                                                  parameters.c_str(), nullptr, SW_HIDE)) <= 32)
        throw std::runtime_error("Cannot launch uninstaller");
      return 0;
    }
    if (!args.empty())
      throw std::runtime_error("Unknown setup argument");
    const auto folder = destination();
    auto prompt = title + L" " + wide(aoip::version) + L" installieren?\n\nZiel: " +
                  folder.wstring() +
                  L"\n\nInstallation nur für dieses Benutzerkonto. Startmenü-Eintrag und "
                  L"Deinstallation werden eingerichtet.";
    if (product == L"VirtualSoundcard" || product == L"Suite")
      prompt += L"\n\nASIO wird anschließend bewusst in der Anwendung registriert. Der "
                L"experimentelle WDM-Kerneltreiber wird nicht installiert.";
    if (MessageBoxW(nullptr, prompt.c_str(), (title + L" Setup").c_str(),
                    MB_YESNO | MB_ICONQUESTION) != IDYES)
      return 0;
    // Check in-use files before touching an existing installation.
    for (const auto& [relative, id] : files())
      if (std::filesystem::exists(folder / relative)) {
        auto check = CreateFileW((folder / relative).c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                                 nullptr, OPEN_EXISTING, 0, nullptr);
        if (check == INVALID_HANDLE_VALUE)
          throw std::runtime_error(
              "Close the installed application and all ASIO hosts before updating");
        CloseHandle(check);
      }
    extract(folder);
    const auto source = std::filesystem::path([] {
      wchar_t p[32768]{};
      GetModuleFileNameW(nullptr, p, 32768);
      return std::wstring(p);
    }());
    if (_wcsicmp(source.c_str(), (folder / setup_name).c_str()) != 0 &&
        !CopyFileW(source.c_str(), (folder / setup_name).c_str(), FALSE))
      throw std::runtime_error("Cannot install uninstaller");
    std::ofstream(folder / L".openaoip-install") << "OpenAoIP " << aoip::version << '\n';
    if (product == L"Suite")
      for (const auto* app :
           {L"OpenAoIP Controller", L"OpenAoIP Virtual Soundcard", L"OpenAoIP Audio Bridge"})
        create_shortcut(folder, app, std::wstring(app) + L".exe");
    else
      create_shortcut(folder);
    register_uninstall(folder);
    MessageBoxW(
        nullptr,
        (title + L" wurde installiert.\n\nDu kannst die Anwendung jetzt über das Startmenü öffnen.")
            .c_str(),
        title.c_str(), MB_OK | MB_ICONINFORMATION);
    return 0;
  } catch (const std::exception& e) {
    MessageBoxW(nullptr, wide(e.what()).c_str(), (title + L" Setup").c_str(), MB_OK | MB_ICONERROR);
    return 1;
  }
}
