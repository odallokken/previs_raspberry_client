# Previs Raspberry Pi Client

A headless [Pexip Infinity](https://www.pexip.com/) video client for the
**Raspberry Pi 4**, built on the
[Pexip Pulse SDK](https://github.com/pexip/doppler).

When the Raspberry Pi boots, the client automatically dials into the Virtual
Meeting Room (VMR) defined in `config.yaml` and stays there, reconnecting
whenever the call drops.

---

## What you need

| Item | Details |
|------|---------|
| Raspberry Pi 4 | Model B, 2 GB RAM or more |
| MicroSD card | 16 GB or larger (Class 10 / A1 recommended) |
| USB camera | Any V4L2-compatible webcam (e.g. Logitech C920) |
| USB headset or speaker/mic | Any ALSA-compatible audio device |
| HDMI display | Optional — the client is headless but useful for debugging |
| Network | Ethernet (recommended) or Wi-Fi |

---

## 1. Flash Ubuntu Server 22.04 or 24.04 (64-bit) to the SD card

The easiest way is the official **Raspberry Pi Imager**:

1. Download **Raspberry Pi Imager** from
   <https://www.raspberrypi.com/software/>.
2. Click **Choose OS → Other general-purpose OS → Ubuntu →
   Ubuntu Server 22.04 LTS (64-bit)**.
3. Click **Choose Storage** and select your SD card.
4. Click the ⚙️ gear icon to pre-configure:
   - Set a hostname (e.g. `previs-room1`).
   - Enable SSH and set a username / password.
   - Configure Wi-Fi if you are not using Ethernet.
5. Click **Write**.

> **Why Ubuntu?** The Pexip Pulse SDK ships pre-built `.deb` packages for
> Ubuntu, which makes installation straightforward without needing to
> cross-compile.

---

## 2. Boot and connect

Insert the SD card, connect the Pi to power and your network, then SSH in:

```bash
ssh ubuntu@<pi-ip-address>
```

---

## 3. Clone this repository

```bash
git clone https://github.com/odallokken/previs_raspberry_client.git
cd previs_raspberry_client
```

---

## 4. Edit the configuration

Edit the `config.yaml` that sits in the repository you just cloned — that is
the directory you are already in after step 3 (for example
`~/previs_raspberry_client/config.yaml`):

```bash
cd ~/previs_raspberry_client   # the clone from step 3
nano config.yaml
```

> **Which `config.yaml`?** There are two copies once the installer has run:
> the one in the cloned repository is the template you edit *before* the
> installation, and `/etc/previs-client/config.yaml` is the live file the
> service actually reads. After installing, edit
> `/etc/previs-client/config.yaml` (see
> [Update the configuration](#update-the-configuration)) — changes to the
> repository copy have no effect, and re-running the installer will not
> overwrite an existing `/etc/previs-client/config.yaml`.

| Key | Description | Example |
|-----|-------------|---------|
| `server` | Hostname or IP of your Pexip Infinity Conferencing Node | `vc.example.com` |
| `vmr` | The Virtual Meeting Room alias to dial into | `meet.boardroom` |
| `display_name` | Name shown for this device in the participant list | `Raspberry Pi - Room 1` |
| `pin` | Host or guest PIN (leave empty string `""` if none) | `1234` |
| `reconnect_delay_seconds` | Seconds to wait before re-dialling after a call drops (minimum 5) | `10` |

Example `config.yaml`:

```yaml
server: "vc.example.com"
vmr: "meet.boardroom"
display_name: "Boardroom Pi"
pin: ""
reconnect_delay_seconds: 10
```

---

## 5. Run the installer

The installer script handles everything: SDK installation, build, system user
creation, and systemd service setup.

```bash
sudo bash scripts/install.sh
```

This will:
1. Install build tools and media packages (`cmake`, `libyaml-cpp-dev`,
   `pulseaudio`, `alsa-utils`, `v4l-utils`, …).
2. Download and install the Pexip Pulse SDK `.deb` packages for this machine's
   architecture (see [Where the Pulse SDK comes from](#where-the-pulse-sdk-comes-from)).
3. Build the `previs-client` binary from source.
4. Install the binary to `/usr/local/bin/previs-client`.
5. Copy `config.yaml` to `/etc/previs-client/config.yaml`.
6. Install the systemd service units to `/etc/systemd/system/`
   (`previs-client.service` plus `previs-pulseaudio.service`, a system-wide
   PulseAudio daemon — see [Audio: why a system-wide PulseAudio](#audio-why-a-system-wide-pulseaudio)).
7. Create a dedicated `previs` system user and add it to the `audio`, `video`
   and `pulse-access` groups.
8. Enable and start the `previs-pulseaudio` and `previs-client` services.

---

## 6. Verify the client is running

```bash
# Show current service status
sudo systemctl status previs-client

# Follow live log output
sudo journalctl -fu previs-client
```

You should see output similar to:

```
[client] Loading config from /etc/previs-client/config.yaml
[client] Connecting to vc.example.com / meet.boardroom as "Boardroom Pi" ...
[pulse]   0%  Resolving server address...
[pulse]  40%  Registering with conference...
[pulse] 100%  Connected.
[pulse] Conference status: Connected
[client] In call. Waiting for disconnect...
```

---

## Day-to-day operations

### Update the configuration

The running service reads `/etc/previs-client/config.yaml`, **not** the copy in
the cloned repository:

```bash
sudo nano /etc/previs-client/config.yaml
sudo systemctl restart previs-client
```

### Stop / start the service manually

```bash
sudo systemctl stop previs-client
sudo systemctl start previs-client
```

### Disable auto-start on boot

```bash
sudo systemctl disable previs-client
```

### Re-enable auto-start on boot

```bash
sudo systemctl enable previs-client
```

---

## Architecture

```
┌─────────────────────────────────────┐
│           previs-client             │
│   (reads config.yaml on startup)    │
│                                     │
│  load_config()                      │
│      │                              │
│      ▼                              │
│  pulse_new()                        │
│  install_callbacks()                │
│  connect_default_devices()          │
│      │                              │
│      ▼   (loop)                     │
│  pulse_connect_with_rest_async()  ──┼──► Pexip Infinity
│      │                              │         │
│  wait for disconnect                │    audio + video
│      │                              │         │
│  wait reconnect_delay_seconds       │    ◄────┘
│      └──────────────────────────────┘
└─────────────────────────────────────┘
            │
            │ (systemd unit restarts process on crash)
            ▼
   systemd/previs-client.service
```

The client is entirely headless — no display, no GUI. Video and audio flow
through the Pexip Pulse SDK using the system's default V4L2 camera and ALSA/
PulseAudio devices.

### Audio: why a system-wide PulseAudio

The Pulse SDK's audio backend talks to a PulseAudio server. PulseAudio normally
runs once per *login session*, but `previs-client` runs as the `previs` system
user, which never logs in — so there is no session, no `XDG_RUNTIME_DIR` and no
sound server to connect to. The symptom is a restart loop logging:

```
[pulse:pulse] Failed to connect: Connection refused
[pulse:alsa] failed to detect PCM formats
[pulse:alsa] Got no caps from device: hw:1,0
[pulse:pulse] Failed to connect: Invalid argument
```

(The SDK first tries PulseAudio, then falls back to raw ALSA, which usually
fails too because the devices are busy or expose no usable format.)

The installer therefore sets up `previs-pulseaudio.service`, a system-wide
PulseAudio daemon that runs without a session, and `previs-client.service`
points the SDK at it with `PULSE_SERVER=unix:/run/pulse/native`. The `previs`
user is added to `pulse-access` so it is allowed to connect.

```bash
sudo systemctl status previs-pulseaudio
sudo -u previs PULSE_SERVER=unix:/run/pulse/native pactl info   # should list sinks/sources
```

If the machine uses PipeWire instead of PulseAudio, disable the PipeWire
services (`systemctl --global disable pipewire pipewire-pulse wireplumber`) or
keep them and point `PULSE_SERVER` at PipeWire's socket instead — the two must
not both own the sound devices.

---

## Building manually (without the installer)

If you prefer to build step by step:

```bash
# 1. Install the Pulse SDK (DEBS = the directory holding the .deb packages;
#    on amd64 you can clone https://github.com/pexip/doppler.git and use
#    /tmp/doppler/sdk/linux/debs, on arm64 use your own packages).
#    Recent SDK releases are a single 'pexninja' package that installs to
#    /opt/pexninja; older releases were three packages installing to /opt/pexip.
DEBS=/home/previs/pulse-debs
sudo dpkg -i "$DEBS"/pexninja_*.deb        # new single-package SDK
# ...or, for the older split packages:
# sudo dpkg -i "$DEBS"/libpexcommon_*.deb \
#              "$DEBS"/libpexpulse_*.deb   \
#              "$DEBS"/libpexpulse-dev_*.deb
sudo apt-get install -f

# 2. Install build dependencies
sudo apt-get install -y cmake build-essential libyaml-cpp-dev \
                        libglfw3-dev libgl1-mesa-dev

# 3. On arm64 only: supply the Arm Performance Libraries the SDK was built
#    against (skip if libamath.so / libastring.so are already installed).
sudo gcc -shared -fPIC -O2 -Wl,-soname,libamath.so \
     -o /opt/pexninja/lib/libamath.so sdk/compat/armpl_compat.c -lm
echo '' | sudo gcc -shared -fPIC -x c - -Wl,-soname,libastring.so \
     -o /opt/pexninja/lib/libastring.so

# 4. Build
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)

# 5. Run with a local config
PEX_BASE_PATH=/opt/pexninja ./build/previs-client config.yaml
```

---

## Where the Pulse SDK comes from

The client links against the closed-source **Pexip Pulse SDK**, shipped as
`.deb` packages. They are far too large to commit to git, so the installer
fetches them. It looks in these places, in order, and uses the first that has
packages matching the machine's architecture (`arm64` on a Raspberry Pi,
`amd64` on a PC):

| # | Source | When it applies |
|---|--------|-----------------|
| 1 | `PULSE_DEB_DIR=/path/to/debs` | You already have the packages on the machine |
| 2 | `sdk/debs/` inside this repository | You copied the packages in; the directory is git-ignored |
| 3 | The URL configured in `sdk/pulse-sdk.conf` | **The normal customer path** — downloaded automatically |
| 4 | A clone of [pexip/doppler](https://github.com/pexip/doppler) | Fallback; upstream publishes **amd64 only** |

### Headers: the runtime package has none

The current `pexninja` package is **runtime only** — it installs
`libpexpulse.so` and its private siblings under `/opt/pexninja/lib` and nothing
else. There is no `pexninja-dev` companion, so `pexpulse/pulse.h` is missing and
the client cannot be compiled against the package alone.

The installer solves this by copying the headers out of the public
[pexip/doppler](https://github.com/pexip/doppler) repository, which checks them
into the tree at `sdk/linux/opt/pexip/include/pexpulse/` (~400 KB), and
installing them next to the runtime at `<prefix>/include`. This happens
automatically; no action is needed.

Pulse exposes a plain C API (`extern "C"`, no C++ name mangling or class
layouts), so a version difference between the doppler headers and the installed
runtime is tolerated. If a build ever fails with an undefined `pulse_*` symbol
at link time, the headers are newer than the runtime — check which symbols the
runtime actually exports:

```bash
nm -D --defined-only /opt/pexninja/lib/libpexpulse.so | grep ' pulse_'
```

### arm64: the SDK needs the Arm Performance Libraries

The arm64 build of the SDK is compiled with the Arm Compiler for Linux, so its
libraries record `NEEDED libamath.so` / `NEEDED libastring.so` and reference
symbols such as `armpl_vsinq_f32`. Those come from the Arm Performance
Libraries, which are shipped neither with the `pexninja` package nor with
Ubuntu, so a plain build fails with:

```
/usr/bin/ld: warning: libamath.so, needed by /opt/pexninja/lib/libpexpulse.so, not found
/usr/bin/ld: /opt/pexninja/lib/libpexpulse.so: undefined reference to `armpl_vsinq_f32'
```

The installer handles this automatically: if neither library is present it
compiles `sdk/compat/armpl_compat.c` into `<prefix>/lib/libamath.so` — the same
vector math entry points implemented lane by lane on top of the standard C
math library — together with an empty `libastring.so` (the real one only
replaces plain C string routines that glibc already provides). If the genuine
Arm Performance Libraries are installed, they are used instead and nothing is
built.

For customers this should be invisible: clone the repository and run the
installer. That only works once the maintainer has published the packages, as
described next.

### Publishing the Pulse SDK (maintainers)

Do this once per SDK version, on any machine with the packages and the `gh`
CLI installed:

1. Collect the packages for every architecture you support — for the Raspberry
   Pi that is the single `pexninja_..._arm64.deb` (recent SDK releases), or the
   older `libpexcommon`, `libpexpulse` and `libpexpulse-dev` `_arm64.deb`
   trio.
2. Note their checksums:

   ```bash
   sha256sum *.deb
   ```

3. Create a release on this repository and attach the packages:

   ```bash
   gh release create pulse-sdk-1.0.17841 *.deb \
       --repo odallokken/previs_raspberry_client \
       --title "Pexip Pulse SDK 1.0.17841" \
       --notes "Pulse SDK packages used by scripts/install.sh"
   ```

4. Fill in `sdk/pulse-sdk.conf` with the release URL, the exact file names and
   the checksums from step 2, then commit it:

   ```bash
   PULSE_SDK_BASE_URL="https://github.com/odallokken/previs_raspberry_client/releases/download/pulse-sdk-1.0.17841"
   PULSE_SDK_FILES_arm64="pexninja_....ubuntu2404_arm64.deb"
   PULSE_SDK_SHA256_arm64="<sum1>"
   ```

   The file names and checksums are space separated and must be listed in the
   same order.

From then on, every customer install is just `git clone` + `sudo bash
scripts/install.sh`.

> **Licensing:** the Pulse SDK is Pexip's closed-source software. Confirm with
> Pexip that you may redistribute the packages before publishing them, and use
> a private repository or an internal file server if you may not.

---

## Repository layout

```
previs_raspberry_client/
├── config.yaml                  User-editable configuration
├── CMakeLists.txt               CMake build definition
├── src/
│   └── main.cpp                 Application source (C++17)
├── sdk/
│   ├── pulse-sdk.conf           Where the installer downloads the Pulse SDK from
│   ├── compat/
│   │   └── armpl_compat.c       Arm Performance Libraries replacement (arm64)
│   └── debs/                    Optional local Pulse SDK .deb packages (git-ignored)
├── systemd/
│   ├── previs-client.service    systemd unit file
│   └── previs-pulseaudio.service  system-wide PulseAudio daemon unit
└── scripts/
    └── install.sh               One-shot installer script
```

---

## Troubleshooting

| Problem | Check |
|---------|-------|
| Service fails to start | `sudo journalctl -xe -u previs-client` for the full error |
| Camera not found | `ls /dev/video*` — ensure the camera is connected; `v4l2-ctl --list-devices` |
| No audio | `aplay -l` to list playback devices; `arecord -l` to list capture devices |
| `[pulse:pulse] Failed to connect: Connection refused` and a restart loop | No sound server for the headless `previs` user. Check `sudo systemctl status previs-pulseaudio` and that `previs` is in the `pulse-access` group (`id previs`) — see [Audio: why a system-wide PulseAudio](#audio-why-a-system-wide-pulseaudio) |
| `[pulse:alsa] Could not open device hw:X,0` | Another process (a desktop PulseAudio/PipeWire instance) already owns the device. Stop it, or leave device handling to `previs-pulseaudio` only |
| Cannot reach server | `ping <server>` — check network and firewall rules (Pexip uses TCP 443 and UDP 3478/3479) |
| Wrong PIN | Edit `/etc/previs-client/config.yaml` and restart the service |
| `dpkg-dev : Depends: bzip2 but it is not installable` during install | The image has an incomplete apt configuration (the `-updates` pocket and/or the `universe` component is missing). The installer now enables them automatically; if it still fails, check `/etc/apt/sources.list` and `/etc/apt/sources.list.d/`, then run `sudo apt-get update && sudo apt-get -f install` |
| SDK packages not found | Check that the `.deb` files exist in `PULSE_DEB_DIR` / `sdk/debs/` and are named `<package>_<version>_<arch>.deb` (for example `pexninja_1.0.18250...ubuntu2404_arm64.deb`) |
| `undefined reference to armpl_...` / `libamath.so ... not found` | The arm64 SDK needs the Arm Performance Libraries. Re-run `sudo bash scripts/install.sh`, which builds replacements into the SDK's `lib/` directory — see [arm64: the SDK needs the Arm Performance Libraries](#arm64-the-sdk-needs-the-arm-performance-libraries) |
| `fatal error: pexpulse/pulse.h: No such file` | The runtime package ships no headers and the installer could not reach github.com to fetch them from the doppler repository — see [Headers: the runtime package has none](#headers-the-runtime-package-has-none) |
| Link errors mentioning `pa_*` or `pw_*` symbols | The Pulse runtime's own dependencies (`libpulse0`, `libpipewire-0.3-0`, `libasound2`, `libX11`, ...) are missing. Run `sudo apt-get install -f` to let the SDK package pull them in |
| `package architecture (amd64) does not match system (arm64)` | The public doppler repository only ships amd64 packages. An arm64 SDK release must be configured in `sdk/pulse-sdk.conf` — see [Where the Pulse SDK comes from](#where-the-pulse-sdk-comes-from) |
| `Failed to download ...` during install | Check the URL in `sdk/pulse-sdk.conf` is reachable from the Pi (`curl -I <url>`) and that any proxy is configured |
| `Checksum mismatch for ...` | The published package changed or the download was truncated. Re-run the installer; if it persists, update the checksums in `sdk/pulse-sdk.conf` |