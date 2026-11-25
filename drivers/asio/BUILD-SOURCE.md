# Rebuilding the ASIO component

This directory contains the complete corresponding source for OpenAoIPASIO64.dll,
including the shared-memory transport, ASIO SDK headers, license notices and CMake build script.
The driver is GPL-3.0-only. The transport's original MIT permission remains in effect.

Use Windows x64, Visual Studio's Desktop development with C++ workload and CMake 3.20+:

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
```

The output is `build/Release/OpenAoIPASIO64.dll`. No registration is required to build it.
The independent Virtual Soundcard application creates the shared-memory audio buffers;
the DLL can then be selected by an ASIO host after deliberate administrator registration.
All project-specific component sources were modified for OpenAoIP 0.5.0 in November 2026.
