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

```bash
nano config.yaml
```

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
2. Clone the [doppler](https://github.com/pexip/doppler) repository to
   download the Pexip Pulse SDK `.deb` packages and install them.
3. Build the `previs-client` binary from source.
4. Install the binary to `/usr/local/bin/previs-client`.
5. Copy `config.yaml` to `/etc/previs-client/config.yaml`.
6. Install the systemd service unit to `/etc/systemd/system/`.
7. Create a dedicated `previs` system user.
8. Enable and start the `previs-client` service.

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

---

## Building manually (without the installer)

If you prefer to build step by step:

```bash
# 1. Install the Pulse SDK (from the doppler repository)
git clone --depth 1 https://github.com/pexip/doppler.git /tmp/doppler
sudo dpkg -i /tmp/doppler/sdk/linux/debs/libpexcommon_*.deb \
             /tmp/doppler/sdk/linux/debs/libpexpulse_*.deb   \
             /tmp/doppler/sdk/linux/debs/libpexpulse-dev_*.deb
sudo apt-get install -f

# 2. Install build dependencies
sudo apt-get install -y cmake build-essential libyaml-cpp-dev \
                        libglfw3-dev libgl1-mesa-dev

# 3. Build
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)

# 4. Run with a local config
PEX_BASE_PATH=/opt/pexip ./build/previs-client config.yaml
```

---

## Repository layout

```
previs_raspberry_client/
├── config.yaml                  User-editable configuration
├── CMakeLists.txt               CMake build definition
├── src/
│   └── main.cpp                 Application source (C++17)
├── systemd/
│   └── previs-client.service    systemd unit file
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
| Cannot reach server | `ping <server>` — check network and firewall rules (Pexip uses TCP 443 and UDP 3478/3479) |
| Wrong PIN | Edit `/etc/previs-client/config.yaml` and restart the service |
| SDK packages not found | Check that `sdk/linux/debs/*.deb` exist in the doppler repository clone |