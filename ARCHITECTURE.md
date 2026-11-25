# OpenAoIP 0.6 architecture and validation boundaries

| Module | Responsibility |
|---|---|
| `aoip_audio` | Existing PCM/RTP and jitter buffers, hardware I/O, new native float endpoints and adaptive SPSC buffers |
| `aoip_network` | Sockets, network initialization and discovery |
| `aoip_core` | Registration/plans, routing/persistence and sender/receiver lifecycle |
| `aoip_session` | Shared discovery, network-node ownership, PCM conversion and measured diagnostics |
| `aoip_bridge_engine` | Multiple WASAPI nodes, patches, meters, reconnection and drift buffers |
| `aoip_virtual_bridge` | Existing ASIO audio-memory ABI, rings and optional buffer-preference sidecar |
| `aoip_vsc_engine` | ASIO and experimental WDM driver transport through the shared session |
| `aoip_config` | Bounded reads and atomic configuration replacement |
| `aoip_engine_host` | Headless per-user engine ownership and authenticated local control |
| `aoip_native_ui` | Shared Win32/GDI layout, controls, DPI and matrix view model |

Audio flows directly between peers. The Controller owns its server and persistence. Existing
CLI, web controls, ASIO driver and WDM protocol remain available. Bridge/VSC share the network
session implementation; their frontends contain no audio callbacks.

## Per-user process and service decision

VSC and Bridge launch or connect to `OpenAoIP Engine.exe` in the current Windows user session.
A named instance lock and per-user/session rendezvous file prevent duplicate owners of the
same local endpoint. Each session has a unique ID; a duplicate active ID is rejected.
The retained ASIO memory owner check rejects competing VSC/ASIO/WDM backends.

The frontend explicitly stops its own audio session on close. Other active sessions continue.
An idle engine exits when released or after 60 seconds without client requests. It can run
without a GUI: `"OpenAoIP Engine.exe" --bridge-config bridge.json` (Ctrl+C stops it).
The GUI starts it without a console. No autostart, SCM service, firewall change or kernel
installation occurs implicitly.

An SCM service is technically possible but deferred. Microsoft documents that
[services cannot directly interact with the user desktop](https://learn.microsoft.com/en-us/windows/win32/services/interactive-services).
[WASAPI loopback can run in Session 0 and capture other sessions](https://learn.microsoft.com/en-us/windows/win32/coreaudio/loopback-recording),
so Session 0 does not inherently prohibit audio capture. A per-user engine is currently chosen
to preserve the working `Local\\` ASIO mapping and user-session device/privacy boundaries,
without elevated service privileges. A future service needs explicit session agents, SID ACLs,
logon/reconnect and cross-session ownership semantics.

The local API uses a random loopback TCP port and a 256-bit system-generated token stored in
the user's LocalAppData. Browser Origin headers are rejected. This API is separate from the
still-unencrypted trusted-LAN Controller API.

## Audio path

Bridge uses WASAPI shared mode, float callbacks and stable Windows endpoint IDs. Loopback
selects a rendering endpoint. Each node has a bounded SPSC queue. Device construction, stop
and destruction occur on the control thread; retired callback state remains alive until
network readers release it. Audio callbacks do not allocate or take configuration mutexes.

Miniaudio converts native sample rates with a linear resampler and low-pass filter. A continuous
interpolation servo compensates residual device/network clock drift up to ±2000 ppm, primes
a configurable buffer, zeros underflow, counts loss and trims stale audio after interruptions.
This is a prototype resampler, not a mastering converter or shared clock synchronizer.

Each TX has one local source. Capture channels may fan out to multiple TX channels. Playback
does not sum several RX channels into a local output. One local device/direction has one entry
with multiple patches. Increasing the network channel count does not expose ASIO-only channels
of a physical interface through WASAPI.

RTP retains 240-frame L16 stereo packets at 44.1/48/96 kHz with independently routable sides.
Odd final channels have a silent padded side. Status uses real sender/receiver counters.
Bandwidth is observed RTP traffic, excluding IP/UDP/Ethernet overhead and control traffic.

## Experimental boundaries

PTP and grandmaster election are unimplemented. Internal/free-running is the only accepted
clock source; PTP selection returns an explicit error. Clock Locked is never fabricated.
Bridge drift correction does not create phase coherence across computers.

ASIO remains a user-mode x64 COM driver and needs no Windows test mode. The audio-memory ABI
is preserved; preferred/active buffer settings use a separate optional mapping.
WDM retains protocol v2 and 16 stereo pairs per direction fixed at 48 kHz. GUI start requires
a compatible driver and Windows endpoints. Production signing, kernel runtime, HLK, power/PnP
and long-duration tests remain open. Regular installers never enable test signing or trust certificates.

## Verification

- Retained RTP/jitter, fan-out/multi-device, outage/restart, I/O adapter, ownership and three-rate ASIO waveform tests.
- New local-API/UDP bridge integration with a null native test backend, both directions,
  playback meters, missing-device errors, authentication and duplicate ownership.
- Channel aliases, filtering, atomic presets and invalid-preset rollback.
- ±500 ppm synthetic clock drift over simulated three-minute intervals, concurrent buffers and stale trimming.
- Real Win32 controls in all three apps, endpoint editor, simultaneous Bridge/VSC Start/Stop.
- Four installer extractions, binary hashes and full ASIO corresponding-source/licenses.

Test backends/fixtures are available only to explicit tests or `--preview`. Production runtime
does not fall back to dummy audio. Tests do not establish Serato/DAW compatibility, acoustic
quality, physical hot-unplug recovery or long-duration multi-PC performance. WDM runtime skips
when no accepted driver is installed.
