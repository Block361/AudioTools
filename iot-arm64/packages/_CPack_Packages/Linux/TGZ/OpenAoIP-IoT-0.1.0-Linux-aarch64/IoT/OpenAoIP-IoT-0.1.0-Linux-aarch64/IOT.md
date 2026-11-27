# OpenAoIP IoT 0.1.0

OpenAoIP IoT is a separate, headless Linux product for devices such as a Raspberry Pi 3B.
Its executable is `openaoip-iot`; it does not require a desktop, browser, Windows driver or ASIO.
Use OpenAoIP Controller **0.6.4 or newer** to manage it. The Windows Suite installer does not
install the IoT daemon; the Linux package is installed on the Pi separately.

## Install on the Pi

The supplied **Linux-aarch64** package targets **64-bit Ubuntu Server 26.04**. Check `uname -m`:
it must report `aarch64`. A Windows EXE or Linux x86_64 binary cannot run on the Pi. The ARM64
build is not a claim that physical Raspberry Pi audio, USB hotplug or long-term performance
has been validated. Start with stereo at 48 kHz and Ethernet.

1. Copy the ARM64 `.tar.gz` package to the Pi using SCP, USB or another file-transfer method.
2. On the Pi, install runtime audio support and extract the package:

   ```sh
   sudo apt update
   sudo apt install libasound2t64 alsa-utils libstdc++6
   tar -xzf OpenAoIP-IoT-0.1.0-Linux-aarch64.tar.gz
   cd OpenAoIP-IoT-0.1.0-Linux-aarch64
   sudo sh ./deploy/install.sh
   ```

3. Edit the bootstrap configuration if needed:

   ```sh
   sudo nano /var/lib/openaoip-iot/iot.json
   ```

   Set `audio.controller` to the Windows Controller URL, for example `http://192.168.1.10:3210`,
   or leave `auto` for LAN discovery. Use a unique `audio.id`. `audio.interface` is the Pi's local
   IPv4 address or `0.0.0.0` for internal-clock operation. PTP requires the actual LAN address.

4. Copy the generated **management.token** into the Controller when pairing. Keep the file private.
   Start the service explicitly:

   ```sh
   sudo systemctl enable --now openaoip-iot.service
   journalctl -u openaoip-iot.service -f
   ```

The installer creates a dedicated `openaoip-iot` user with audio-group access. It preserves
existing configuration and keys on update. It does not modify firewalls, OS clock settings or
boot security. It stops this product during an explicit update; start it again afterwards.

## Controller management

1. Start OpenAoIP Controller on the same reachable network.
2. Choose **Manage IoT**, select the discovered Pi and enter its private 64-character management key.
3. Choose **Connect / refresh**. The Controller reads the actual device configuration and ALSA inventory.
4. Configure device name, LAN address, TX/RX counts, sample rate, I/O frames, local/network latency,
   PTP role/domain and network-channel names. Comma-separated names cover the configured channel order.
5. Use **Add device / Edit map / Remove** for local capture and playback devices. Assign local physical
   channels to network TX/RX channels and set gain. Multiple devices can run concurrently.
6. **Apply & restart** validates, transfers and persists the settings. Running audio is briefly stopped
   while the configuration is replaced. Invalid settings are rejected before stopping audio. A failed
   save/start retains the previous configuration and attempts to restart the previous audio settings.
7. **Start / Stop** controls audio and saves the desired state for the next boot. The stopped device
   remains discoverable and configurable. Use the normal Routing matrix to connect its channels.

**Refresh reloads settings from the device**, replacing unsaved local edits. **Export audio / Import audio**
works with the audio section only and does not export the key. Import retains this device's ID and
controller bootstrap address. A configuration revision prevents one Controller from silently overwriting
another Controller's changes. A stale revision is rejected until refreshed.

The live status shows running state, software clock, RTP loss, underruns and receive level. The device
API also exposes the full Bridge endpoint meters, native formats, drift/queue counters and PTP diagnosis.
The management key is kept in the Controller dialog's memory, not routing presets or device advertisements.

## Audio capabilities and Linux differences

- Shared Audio Bridge engine: actual bidirectional L16 RTP, multiple local endpoints, 1–32 configured
  network channels per direction, 44.1/48/96 kHz, independent channel maps/gain/names, continuous SRC,
  clock-drift buffers, configurable latency and retry after local-device loss.
- ALSA capture/playback runs without a GUI or user desktop audio session. USB interfaces and microphones
  must actually expose supported ALSA devices. The Pi's physical channel count is determined by its audio
  hardware; configuring 32 network channels does not create 32 physical inputs.
- A Pi with no capture hardware does not gain a microphone from this software. Use a USB microphone,
  USB soundcard or another supported interface. Choose only channels available on that device.
- Native system-mix loopback in the shared library is WASAPI-only. On Linux, configure an ALSA loopback
  PCM separately and select its **capture** endpoint. IoT rejects Windows `mode: "loopback"` entries.
  Serato/Windows applications can send through their Windows Bridge to the Pi over the normal matrix.
- `clockSource` supports `internal`, `ptp-monitor` (software follower) and `ptp-grandmaster`. One manually
  chosen GM per domain. This retains the current experimental frequency discipline and Holdover behavior;
  it does not add sample-accurate presentation or hardware timestamping. See [PTP](./PTP.md).

## Configuration file

Create a complete file with a unique key instead of copying a shared example key:

```sh
./bin/openaoip-iot --init --config ./iot.json
./bin/openaoip-iot --audio-devices
./bin/openaoip-iot --check-config --config ./iot.json
./bin/openaoip-iot --config ./iot.json
```

`--init` never overwrites an existing file. The generated file contains `product`, `schemaVersion: 1`,
`revision`, `autoStart`, `management` (`bind`, `port`, `token`) and the shared Bridge `audio` configuration.
The initial state is **stopped**, with a two-channel default playback mapping, so a freshly installed
device can be configured before it opens audio hardware. Management uses TCP **32110** by default.
The file is written atomically; Linux permissions are owner-read/write only.

[Example audio settings](./examples/iot-audio.json) can be imported in the Controller. Replace default
devices with IDs from the Pi's inventory, for example `alsa:...`, when a fixed device is wanted.
The example assumes a stereo capture device; a mono microphone needs a channel-1-only map or explicit
duplication of local channel 1 to TX 1/2. A default-device selection follows ALSA's default PCM.

Device ID, controller bootstrap URL, management bind/port and management key are local-file settings.
Stop the service before editing those and restart afterwards. Remote edits cannot redirect the
management service or change its key. Do not edit the file concurrently with Controller changes.

## Network and diagnostics

Discovery uses UDP 32100; the Controller normally uses TCP 3210; RTP uses dynamically allocated UDP
ports; PTP uses UDP 319/320. The management API uses a bearer key over **HTTP**, intended for a trusted
audio LAN or secured tunnel, not direct Internet exposure. The key is not a TLS substitute.
The service remains manageable if the Controller or an audio device disappears. Previously applied
audio routes continue across Controller loss; management changes still require reachability to the Pi.

```sh
systemctl status openaoip-iot
journalctl -u openaoip-iot -n 100 --no-pager
sudo -u openaoip-iot /usr/local/bin/openaoip-iot --audio-devices
aplay -l
arecord -l
```

If the Pi is not discovered, check the bootstrap URL, IP binding, cabling and allowed ports.
If it is visible but pairing fails, check its key and management port. Hardware errors are reported;
there is no automatic dummy-device or null-audio fallback in the production executable.

## Build from source

```sh
sudo apt install build-essential cmake libasound2t64 alsa-utils
cmake -S . -B build/iot -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build build/iot --target openaoip_iot -j1
cmake --install build/iot --component IoT --prefix "$PWD/build/iot-package"
```

Use `-j1` on a memory-limited Pi 3B. No network fetch of C++ dependencies is required; they are vendored.
For an ARM64 cross-build on an x86 Ubuntu machine with `g++-aarch64-linux-gnu`, add
`-DCMAKE_TOOLCHAIN_FILE=cmake/linux-arm64.cmake`. Native tests use a separately enabled test audio backend;
they are not substitutes for physical Pi/USB-interface tests. Do not run an x86_64 build on ARM64.

## Management API v1

Every request needs `Authorization: Bearer <management.token>`; browser Origin requests are rejected.
All successful responses identify `product`, `id`, `version` and `schemaVersion`.

| Endpoint | Operation |
|---|---|
| `GET /v1/config` | Audio settings, desired startup state and revision; no secret |
| `GET /v1/audio-devices` | Actual ALSA devices and local interfaces |
| `GET /v1/status` | Actual Bridge/network/clock/diagnostic status |
| `POST /v1/config` | `{ "revision": N, "audio": { ... } }`; validate/apply/save |
| `POST /v1/run` | `{ "revision": N, "running": true/false }`; start/stop/save |

Mutations increment the revision. Wrong keys return 403; concurrent edits return 409; invalid settings
return 400. Check actual status after an unknown network result before retrying a mutation.

## Removal

```sh
sudo systemctl disable --now openaoip-iot
sudo rm /etc/systemd/system/openaoip-iot.service /usr/local/bin/openaoip-iot
sudo systemctl daemon-reload
```

The configuration directory is retained for recovery; remove it separately only when no longer needed.
The existing Windows suite and its ASIO/WDM drivers are independent of this product.
