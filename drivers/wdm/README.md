# Experimental OpenAoIP WDM / WaveRT driver

This is a development implementation, not a released Windows audio driver. It has
been compiled against the Windows 11 26100 WDK and its INF passes InfVerif `/w`, but has not been loaded or tested in
kernel mode. Do not infer runtime safety, endpoint enumeration or DAW compatibility
from compilation. Default OpenAoIP installers do not install or stage this driver.

## Implemented pieces

- PortCls render/capture WaveRT miniports and topology filters.
- 16 independent stereo endpoint pairs per direction, PCM16 at fixed 48 kHz.
- Configurable 1–32 network channels mapped onto those stable Windows pairs; unused sides are silent.
- Endpoint association, friendly names, OEM formats, connected-jack reporting and event-driven mode.
- Software position and notification events, bounded per-pair queues and bounded DPC catch-up.
- Bounded kernel queues with a buffered device-control interface to the user-mode engine.
- Ownership bound to an open file object and originating process ID; cleanup releases queues.
- User-mode WDM client and actual MMDevice endpoint enumeration in the native Soundcard application.
- A separate root-device installer that requires trusted catalog membership for INF and SYS files.
- Root-enumerated INF and an optional project-local WDK build target.

The software timer is not a PTP clock. Production-quality power/PnP handling,
Windows Audio Engine endpoint enumeration, concurrent
state transitions, DPC timing and teardown all require dedicated validation and
potential changes before release. Current DPC work and queue locking also need
latency profiling at 32 channels / 96 kHz.

## Development build

Provide an official x64 WDK through `OPENAOIP_WDK_ROOT` (a root with `Include` and `Lib`)
and a matching installed Windows SDK. The local development environment used
Microsoft.Windows.WDK.x64 10.0.26100.6584. The toolkit is not redistributed.

```powershell
cmake -S . -B build/desktop -DOPENAOIP_WDK_ROOT=C:/path/to/wdk/c
cmake --build build/desktop --config Release --target OpenAoIPWdm
```

Outputs: `build/desktop/wdm-development/OpenAoIPWdm.sys` and `OpenAoIPWdm.inf`.
The INF targets Windows 11 x64 build 26100 or newer. The target does not sign, install, register or load anything. A release requires INF
validation, a signed catalog/binary, Windows driver signing and dedicated test machines.
The regular build/package scripts change no Secure Boot, test-signing or device-installation state.
For the optional testsigned VirtualBox package, see [VM test instructions](VM-TEST.md).
Its explicit `Prepare` action trusts the test certificate and enables TESTSIGNING only after
detecting a VirtualBox guest. It never changes Secure Boot or BitLocker automatically.

The optional `-Wdm -Package` switches to the root build helper additionally run InfVerif and
Inf2Cat and create a clearly named UNSIGNED development ZIP, including a true WASAPI/RTP
runtime test. See [Windows audio](../../WINDOWS-AUDIO.md). The installer helper changes
the device tree only when a user explicitly runs its install/remove action; check mode is read-only.

## Required validation before distribution

Use a disposable Windows test environment with snapshots and kernel debugging.
Validate the INF and run static driver analysis, then Driver Verifier / relevant audio
HLK tests. Exercise render and capture in WASAPI shared/exclusive mode, channel and
rate changes, idle/resume, PnP removal, client crashes, invalid IOCTL lengths, starvation,
and long-duration bidirectional audio across physical machines. Confirm the user-mode
application reports real endpoint and queue status. Only promote WDM to supported
after these checks pass and deployment is signed.

This code is MIT-licensed as part of OpenAoIP. Microsoft WDK headers and libraries
remain under their separate Microsoft terms.

To build a self-signed VM test package without trusting that certificate on the build host:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File drivers/wdm/package-vm.ps1 -BuildDirectory build/desktop
```

First build the native apps, runtime test and WDM driver. The local development key stays
under the build directory in an access-restricted `private-vm-signing` folder and is ignored
by Git. Only the public certificate goes into the ZIP. Driver embedded signature, catalog
signature and a tampered-driver negative test are checked without adding host trust.
