# Vendored dependencies

These headers are checked in so builds work offline. Their copyright and license notices are retained in the files. Full MIT license texts are included in the repository as `third_party/LICENSE-httplib.txt` and `third_party/LICENSE-json.txt`, and in native application packages under `licenses/`. The project does not use TLS or compression extensions from cpp-httplib.

| Library | Version | License | Vendored file |
|---|---|---|---|
| [cpp-httplib](https://github.com/yhirose/cpp-httplib/tree/v0.58.0) | v0.58.0 | MIT | [httplib.h](./third_party/httplib.h) |
| [JSON for Modern C++](https://github.com/nlohmann/json/tree/v3.12.0) | v3.12.0 | MIT | [json.hpp](./third_party/json.hpp) |
| [miniaudio](https://github.com/mackron/miniaudio/tree/0.11.26) | 0.11.26 | Public domain / MIT No Attribution (choice) | [miniaudio.h](./third_party/miniaudio.h) |

Downloaded from the respective upstream repositories at these fixed tags:

- `https://raw.githubusercontent.com/yhirose/cpp-httplib/v0.58.0/httplib.h`
- `https://raw.githubusercontent.com/nlohmann/json/v3.12.0/single_include/nlohmann/json.hpp`
- `https://raw.githubusercontent.com/mackron/miniaudio/0.11.26/miniaudio.h`

miniaudio's complete dual license is retained in [LICENSE-miniaudio.txt](./third_party/LICENSE-miniaudio.txt). Only its low-level device I/O and conversion support are compiled; engine, decoding, encoding, resource management, node graph and generation components are disabled.

SHA-256:

```text
httplib.h  aa14e7e7bd2703694e0a6b6855af3b8c406102ab1fc56ac905fe33619b31faa5
json.hpp   aaf127c04cb31c406e5b04a63f1ae89369fccde6d8fa7cdda1ed4f32dfc5de63
miniaudio.h 6b376c7fbb1352ba1e0e432ab66069b335ea9405e45cb6c30b97ef0f3c9c3e0e
```

## Native ASIO component

OpenAoIPASIO64.dll and the ASIO host tests use Steinberg ASIO SDK 2.3.4 (2025-10-15)
under its GPLv3 option. Those components are GPL-3.0-only. The independently running
Controller and Virtual Soundcard applications and shared-memory transport retain their
MIT licensing. The SDK was downloaded from the official Steinberg SDK distribution.
ASIO is a Steinberg technology. OpenAoIP is not affiliated with Steinberg or Audinate.

Soundcard packages and their installer carry the complete corresponding ASIO source,
SDK headers, original SDK license, GPL text and a standalone CMake build under
`asio-source/`. The source can be rebuilt with the standard Windows C++ toolchain.

## Optional WDM development build

The Windows Driver Kit is used only to build the optional development driver. Its
headers and libraries are not redistributed in OpenAoIP packages or installers and
remain subject to Microsoft's own terms. The default native build needs only the
Windows SDK; the WDM target is available when a matching WDK is supplied separately.
