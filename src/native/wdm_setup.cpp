// SPDX-License-Identifier: MIT
// Installs only an explicitly selected, trusted catalog package. No certificate or boot policy
// changes.
#include "aoip/system.hpp"
#include "ui.hpp"
#include <fstream>
// Windows catalog declarations depend on wincrypt types.
// clang-format off
#include <wincrypt.h>
#include <mscat.h>
#include <wintrust.h>
#include <softpub.h>
// clang-format on
#include <newdev.h>
#include <setupapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <vector>
namespace {
using namespace aoip::ui;
constexpr GUID media_class{
    0x4d36e96c, 0xe325, 0x11ce, {0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18}};
constexpr wchar_t hardware_id[] = L"Root\\OpenAoIPWdm";
void failed(const char* action) {
  throw std::runtime_error(std::string(action) + " (Windows-Fehler " +
                           std::to_string(GetLastError()) + ")");
}
struct Devices {
  HDEVINFO value = INVALID_HANDLE_VALUE;
  ~Devices() {
    if (value != INVALID_HANDLE_VALUE)
      SetupDiDestroyDeviceInfoList(value);
  }
};
struct Handle {
  HANDLE value = INVALID_HANDLE_VALUE;
  ~Handle() {
    if (value != INVALID_HANDLE_VALUE)
      CloseHandle(value);
  }
};
void verify_member(const std::filesystem::path& catalog, const std::filesystem::path& file) {
  HCATADMIN admin = nullptr;
  if (!CryptCATAdminAcquireContext2(&admin, nullptr, L"SHA256", nullptr, 0))
    failed("Katalogpruefung nicht verfuegbar");
  struct Release {
    HCATADMIN value;
    ~Release() {
      CryptCATAdminReleaseContext(value, 0);
    }
  } release{admin};
  Handle input;
  input.value =
      CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
  if (input.value == INVALID_HANDLE_VALUE)
    failed("Treiberdatei fehlt");
  DWORD count = 0;
  CryptCATAdminCalcHashFromFileHandle2(admin, input.value, &count, nullptr, 0);
  if (!count || count > 128)
    failed("Dateihash nicht verfuegbar");
  std::vector<BYTE> hash(count);
  if (!CryptCATAdminCalcHashFromFileHandle2(admin, input.value, &count, hash.data(), 0))
    failed("Dateihash fehlgeschlagen");
  const wchar_t hex[] = L"0123456789ABCDEF";
  std::wstring tag;
  for (auto b : hash) {
    tag += hex[b >> 4];
    tag += hex[b & 15];
  }
  WINTRUST_CATALOG_INFO info{sizeof(info)};
  info.pcwszCatalogFilePath = catalog.c_str();
  info.pcwszMemberTag = tag.c_str();
  info.pcwszMemberFilePath = file.c_str();
  info.hMemberFile = input.value;
  info.pbCalculatedFileHash = hash.data();
  info.cbCalculatedFileHash = count;
  info.hCatAdmin = admin;
  WINTRUST_DATA trust{sizeof(trust)};
  trust.dwUIChoice = WTD_UI_NONE;
  trust.fdwRevocationChecks = WTD_REVOKE_NONE;
  trust.dwUnionChoice = WTD_CHOICE_CATALOG;
  trust.pCatalog = &info;
  trust.dwStateAction = WTD_STATEACTION_VERIFY;
  trust.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;
  GUID policy = WINTRUST_ACTION_GENERIC_VERIFY_V2;
  const auto result = WinVerifyTrust(nullptr, &policy, &trust);
  trust.dwStateAction = WTD_STATEACTION_CLOSE;
  WinVerifyTrust(nullptr, &policy, &trust);
  if (result != ERROR_SUCCESS)
    throw std::runtime_error(
        "Treiberpaket ist nicht vertrauenswuerdig signiert oder die Datei passt nicht zum Katalog. "
        "Es wurde nichts installiert. Signaturcode: " +
        std::to_string(result));
}
std::filesystem::path verify_package(const std::filesystem::path& source) {
  const auto inf = std::filesystem::canonical(source);
  if (_wcsicmp(inf.filename().c_str(), L"OpenAoIPWdm.inf") != 0)
    throw std::runtime_error("Bitte OpenAoIPWdm.inf aus einem signierten Treiberpaket waehlen.");
  wchar_t value[512]{};
  GetPrivateProfileStringW(L"Version", L"ClassGuid", L"", value, 512, inf.c_str());
  if (_wcsicmp(value, L"{4d36e96c-e325-11ce-bfc1-08002be10318}") != 0)
    throw std::runtime_error("Falsche Treiberklasse.");
  GetPrivateProfileStringW(L"Version", L"CatalogFile", L"", value, 512, inf.c_str());
  if (_wcsicmp(value, L"OpenAoIPWdm.cat") != 0)
    throw std::runtime_error("OpenAoIP-Treiberkatalog fehlt.");
  const auto catalog = inf.parent_path() / L"OpenAoIPWdm.cat";
  if (!std::filesystem::exists(catalog))
    throw std::runtime_error("OpenAoIPWdm.cat fehlt. Der aktuelle Entwicklungsbuild ist nicht "
                             "signiert und kann hier nicht installiert werden.");
  verify_member(catalog, inf);
  verify_member(catalog, inf.parent_path() / L"OpenAoIPWdm.sys");
  return inf;
}
bool ours(HDEVINFO set, SP_DEVINFO_DATA& device) {
  wchar_t ids[4096]{};
  DWORD type = 0;
  if (!SetupDiGetDeviceRegistryPropertyW(set, &device, SPDRP_HARDWAREID, &type,
                                         reinterpret_cast<BYTE*>(ids),
                                         sizeof(ids) - sizeof(wchar_t), nullptr) ||
      type != REG_MULTI_SZ)
    return false;
  for (const auto* id = ids; *id; id += wcslen(id) + 1)
    if (_wcsicmp(id, hardware_id) == 0)
      return true;
  return false;
}
void install(const std::filesystem::path& source) {
  const auto inf = verify_package(source);
  if (!IsUserAnAdmin())
    throw std::runtime_error("Treiberinstallation benoetigt Administratorrechte.");
  Devices existing;
  existing.value = SetupDiGetClassDevsW(&media_class, nullptr, nullptr, DIGCF_PRESENT);
  if (existing.value == INVALID_HANDLE_VALUE)
    failed("Geraeteverwaltung nicht erreichbar");
  bool found = false;
  for (DWORD i = 0;; ++i) {
    SP_DEVINFO_DATA d{sizeof(d)};
    if (!SetupDiEnumDeviceInfo(existing.value, i, &d))
      break;
    if (ours(existing.value, d)) {
      found = true;
      break;
    }
  }
  Devices created;
  SP_DEVINFO_DATA device{sizeof(device)};
  if (!found) {
    created.value = SetupDiCreateDeviceInfoList(&media_class, nullptr);
    if (created.value == INVALID_HANDLE_VALUE ||
        !SetupDiCreateDeviceInfoW(created.value, L"OpenAoIPWdm", &media_class,
                                  L"OpenAoIP Windows Audio", nullptr, DICD_GENERATE_ID, &device))
      failed("Virtuelles Audiogeraet konnte nicht angelegt werden");
    constexpr wchar_t ids[] = L"Root\\OpenAoIPWdm\0";
    if (!SetupDiSetDeviceRegistryPropertyW(created.value, &device, SPDRP_HARDWAREID,
                                           reinterpret_cast<const BYTE*>(ids), sizeof(ids)) ||
        !SetupDiCallClassInstaller(DIF_REGISTERDEVICE, created.value, &device))
      failed("Virtuelles Audiogeraet konnte nicht registriert werden");
  }
  BOOL reboot = FALSE;
  if (!UpdateDriverForPlugAndPlayDevicesW(nullptr, hardware_id, inf.c_str(), INSTALLFLAG_FORCE,
                                          &reboot)) {
    const auto error = GetLastError();
    if (!found)
      SetupDiCallClassInstaller(DIF_REMOVE, created.value, &device);
    SetLastError(error);
    failed("Windows hat das Treiberpaket nicht akzeptiert");
  }
  MessageBoxW(nullptr,
              reboot
                  ? L"OpenAoIP-Windows-Audio wurde installiert. Windows muss neu gestartet werden."
                  : L"OpenAoIP-Windows-Audio wurde installiert. Die Kanalpaare stehen nach "
                    L"erfolgreichem Treiberstart in den Windows-Soundeinstellungen zur Auswahl.",
              L"OpenAoIP Windows Audio", MB_OK | MB_ICONINFORMATION);
}
void remove() {
  if (!IsUserAnAdmin())
    throw std::runtime_error("Treiberentfernung benoetigt Administratorrechte.");
  if (MessageBoxW(nullptr,
                  L"OpenAoIP-Windows-Audiogeraete entfernen? Vorher die Soundcard und alle "
                  L"Audioanwendungen schliessen.",
                  L"OpenAoIP", MB_YESNO | MB_ICONQUESTION) != IDYES)
    return;
  Devices list;
  list.value = SetupDiGetClassDevsW(&media_class, nullptr, nullptr, DIGCF_PRESENT);
  if (list.value == INVALID_HANDLE_VALUE)
    failed("Geraeteverwaltung nicht erreichbar");
  for (DWORD i = 0;; ++i) {
    SP_DEVINFO_DATA d{sizeof(d)};
    if (!SetupDiEnumDeviceInfo(list.value, i, &d))
      break;
    if (ours(list.value, d) && !SetupDiCallClassInstaller(DIF_REMOVE, list.value, &d))
      failed("OpenAoIP-Audiogeraet konnte nicht entfernt werden");
  }
  MessageBoxW(
      nullptr,
      L"OpenAoIP-Audiogeraete wurden entfernt. Das Treiberpaket bleibt im Windows-Treiberspeicher.",
      L"OpenAoIP", MB_OK | MB_ICONINFORMATION);
}
} // namespace
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  Apartment apartment;
  const auto args = arguments();
  const bool check = !args.empty() && args[0] == L"--check";
  aoip::Json report = {{"trusted", false}, {"installed", false}};
  int code = 0;
  try {
    if (check && args.size() >= 2) {
      verify_package(args[1]);
      report["trusted"] = true;
    } else if (args.size() == 2 && args[0] == L"--install")
      install(args[1]);
    else if (args.size() == 1 && args[0] == L"--remove")
      remove();
    else
      throw std::runtime_error("Aufruf: OpenAoIPWdmSetup --install OpenAoIPWdm.inf | --remove | "
                               "--check OpenAoIPWdm.inf [--report Datei.json]");
  } catch (const std::exception& e) {
    code = 1;
    report["error"] = e.what();
    if (!check)
      MessageBoxW(nullptr, wide(e.what()).c_str(), L"OpenAoIP Windows Audio", MB_OK | MB_ICONERROR);
  }
  if (check && args.size() == 4 && args[2] == L"--report") {
    std::ofstream out{std::filesystem::path(args[3])};
    out << report.dump(2);
    if (!out)
      return 2;
  }
  return code;
}
