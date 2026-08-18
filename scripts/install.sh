#!/usr/bin/env bash
# =============================================================================
#  install.sh — one-shot installer for the Previs Pexip Video Client
# =============================================================================
#
#  Run as root (or with sudo) on a freshly flashed Raspberry Pi running
#  Ubuntu Server 22.04 or 24.04 (64-bit / ARM64):
#
#      sudo bash scripts/install.sh
#
#  What this script does:
#    1. Installs OS build dependencies and audio/video packages.
#    2. Installs the Pexip Pulse SDK .deb package(s).  Recent SDK releases are
#       a single 'pexninja_<version>_<arch>.deb'; older ones were
#       libpexcommon + libpexpulse + libpexpulse-dev.  Both are supported.
#       They are normally
#       downloaded automatically from the release configured in
#       sdk/pulse-sdk.conf, so a plain 'git clone' of this repository is all a
#       customer needs.  Packages can also be supplied manually:
#
#           sudo PULSE_DEB_DIR=/path/to/debs bash scripts/install.sh
#
#       or by dropping them into <repo>/sdk/debs/.
#    3. Builds the previs-client binary with CMake.
#    4. Installs the binary, config file, and systemd service.
#    5. Creates a dedicated 'previs' system user.
#    6. Enables and starts the systemd service.
#
#  After installation, edit the configuration file and restart:
#
#      sudo nano /etc/previs-client/config.yaml
#      sudo systemctl restart previs-client
#
# =============================================================================

set -euo pipefail

# The script lives in <repo>/scripts, so the repository root is one level up.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
DOPPLER_REPO="https://github.com/pexip/doppler.git"
DOPPLER_DIR="/tmp/doppler-sdk"
DOWNLOAD_DIR="/tmp/pulse-sdk-debs"
BUILD_DIR="/tmp/previs-client-build"
INSTALL_PREFIX="/usr/local"

# Where the Pulse SDK ends up.  The current 'pexninja' package installs to
# /opt/pexninja; the older libpexpulse packages used /opt/pexip.  The first
# prefix that actually contains the SDK headers is used.
SDK_PREFIXES=("/opt/pexninja" "/opt/pexip")
SDK_PREFIX=""

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'

info()    { echo -e "${GREEN}[install]${NC} $*"; }
warning() { echo -e "${YELLOW}[install]${NC} $*"; }
error()   { echo -e "${RED}[install]${NC} $*" >&2; exit 1; }

# ---------------------------------------------------------------------------
# 0. Privilege check
# ---------------------------------------------------------------------------
if [[ $EUID -ne 0 ]]; then
    error "Please run as root: sudo bash scripts/install.sh"
fi

# ---------------------------------------------------------------------------
# 1. OS dependencies
# ---------------------------------------------------------------------------

# Some Raspberry Pi images ship with an incomplete apt configuration: only the
# release and '-security' pockets are enabled, and/or the 'universe' component
# is missing.  That combination breaks dependency resolution for packages that
# received a security update (a typical symptom is
# "dpkg-dev : Depends: bzip2 but it is not installable"), because the updated
# library is installed while the matching tool is only available in the
# '-updates' pocket that is not configured.  Enable the missing pockets and
# components before installing anything.
ensure_apt_sources() {
    local codename="" changed=0
    if [[ -r /etc/os-release ]]; then
        codename="$(. /etc/os-release && echo "${VERSION_CODENAME:-}")"
    fi
    [[ -n "${codename}" ]] || { warning "Could not detect the Ubuntu codename — skipping apt source check."; return 0; }

    local f
    # deb822 style sources (Ubuntu 24.04 and newer).
    for f in /etc/apt/sources.list.d/*.sources; do
        [[ -f "${f}" ]] || continue
        grep -qE "^Suites:.*(^|[[:space:]])${codename}([[:space:]]|$)" "${f}" || continue
        if ! grep -qE "^Suites:.*${codename}-updates" "${f}"; then
            [[ -f "${f}.previs.bak" ]] || cp "${f}" "${f}.previs.bak"
            sed -i -E "s/^(Suites:.*[[:space:]]?)${codename}([[:space:]]|$)/\1${codename} ${codename}-updates\2/" "${f}"
            changed=1
        fi
        if grep -qE "^Components:" "${f}" && ! grep -qE "^Components:.*universe" "${f}"; then
            [[ -f "${f}.previs.bak" ]] || cp "${f}" "${f}.previs.bak"
            sed -i -E "s/^(Components:.*)$/\1 universe/" "${f}"
            changed=1
        fi
    done

    # Legacy one-line sources.
    for f in /etc/apt/sources.list /etc/apt/sources.list.d/*.list; do
        [[ -f "${f}" ]] || continue
        grep -qE "^deb .*[[:space:]]${codename}[[:space:]]" "${f}" || continue
        if ! grep -qE "^deb .*[[:space:]]${codename}-updates[[:space:]]" "${f}"; then
            [[ -f "${f}.previs.bak" ]] || cp "${f}" "${f}.previs.bak"
            # Duplicate the release line, pointing it at the '-updates' pocket.
            local updates_line
            updates_line="$(sed -nE "s/^(deb .*[[:space:]])${codename}([[:space:]].*)$/\1${codename}-updates\2/p" "${f}" | head -n 1)"
            if [[ -n "${updates_line}" ]]; then
                printf '%s\n' "${updates_line}" >> "${f}"
            fi
            changed=1
        fi
    done

    if (( changed )); then
        info "Enabled missing apt pockets/components for '${codename}'."
    fi
}

ensure_apt_sources

info "Updating package lists..."
apt-get update -y

info "Installing build tools and audio/video packages..."
if ! apt-get install -y \
    build-essential cmake git curl ca-certificates \
    libyaml-cpp-dev \
    libglfw3-dev libgl1-mesa-dev \
    pulseaudio alsa-utils \
    v4l-utils \
    network-manager
then
    error "Failed to install the required packages. This usually means the apt" \
          "sources on this system are incomplete (for example the" \
          "'-updates' pocket or the 'universe' component is missing)." \
          "Check /etc/apt/sources.list and /etc/apt/sources.list.d/, run" \
          "'sudo apt-get update && sudo apt-get -f install', then re-run this script."
fi

# ---------------------------------------------------------------------------
# 2. Pexip Pulse SDK (.deb packages)
# ---------------------------------------------------------------------------

# Print the first known prefix that holds an installed Pulse SDK, or return 1.
detect_sdk_prefix() {
    local prefix
    for prefix in "${SDK_PREFIXES[@]}"; do
        if [[ -f "${prefix}/include/pexpulse/pulse.h" ]]; then
            printf '%s\n' "${prefix}"
            return 0
        fi
    done
    return 1
}

# Download the Pulse SDK packages listed in sdk/pulse-sdk.conf for this
# machine's architecture.  Returns non-zero (without aborting) when no download
# is configured, so the caller can fall back to another source.
download_sdk_debs() {
    local conf="${REPO_DIR}/sdk/pulse-sdk.conf"
    [[ -f "${conf}" ]] || return 1

    # The config file only ever assigns plain strings to known variables.
    PULSE_SDK_BASE_URL=""
    # shellcheck source=/dev/null
    . "${conf}"

    [[ -n "${PULSE_SDK_BASE_URL}" ]] || return 1

    local files_var="PULSE_SDK_FILES_${DPKG_ARCH}"
    local sha_var="PULSE_SDK_SHA256_${DPKG_ARCH}"
    local files=(${!files_var-}) sums=(${!sha_var-})

    if (( ${#files[@]} == 0 )); then
        error "sdk/pulse-sdk.conf configures a download URL but lists no" \
              "packages for this machine's architecture (${DPKG_ARCH})." \
              "Set ${files_var} in that file, or supply the packages with" \
              "'sudo PULSE_DEB_DIR=/path/to/debs bash scripts/install.sh'."
    fi

    if (( ${#sums[@]} && ${#sums[@]} != ${#files[@]} )); then
        error "sdk/pulse-sdk.conf lists ${#sums[@]} checksums for" \
              "${#files[@]} packages (${DPKG_ARCH}) — they must match one to one."
    fi

    info "Downloading the Pulse SDK (${DPKG_ARCH}) from ${PULSE_SDK_BASE_URL}..."
    rm -rf "${DOWNLOAD_DIR}"
    mkdir -p "${DOWNLOAD_DIR}"

    local i file target
    for i in "${!files[@]}"; do
        file="${files[${i}]}"
        [[ "${file}" != */* ]] || error "Invalid package name '${file}' in sdk/pulse-sdk.conf" \
                                        "— list file names only, not paths."
        target="${DOWNLOAD_DIR}/${file}"
        curl -fL --progress-bar --retry 3 --retry-delay 2 -o "${target}" \
            "${PULSE_SDK_BASE_URL%/}/${file}" \
            || error "Failed to download ${file} from ${PULSE_SDK_BASE_URL%/}." \
                     "Check the URL in sdk/pulse-sdk.conf and this machine's" \
                     "network/proxy settings."

        if (( ${#sums[@]} )); then
            printf '%s  %s\n' "${sums[${i}]}" "${target}" | sha256sum -c - > /dev/null \
                || error "Checksum mismatch for ${file} — the download is" \
                         "corrupt or the published file has changed."
        fi
    done

    if (( ${#sums[@]} == 0 )); then
        warning "No checksums configured in sdk/pulse-sdk.conf — downloads were not verified."
    fi

    info "Pulse SDK downloaded to ${DOWNLOAD_DIR}"
    return 0
}

if SDK_PREFIX="$(detect_sdk_prefix)"; then
    info "Pulse SDK already installed at ${SDK_PREFIX} — skipping."
else
    DPKG_ARCH="$(dpkg --print-architecture)"

    # Where do the .deb packages come from?  In order of preference:
    #   1. PULSE_DEB_DIR    — a directory you already have the packages in.
    #   2. <repo>/sdk/debs  — packages placed alongside this repository.
    #   3. sdk/pulse-sdk.conf — downloaded from the URL configured there.  This
    #      is the normal customer path: 'git clone' + run this script.
    #   4. The doppler repository (upstream; amd64 only at the time of writing).
    if [[ -n "${PULSE_DEB_DIR:-}" ]]; then
        [[ -d "${PULSE_DEB_DIR}" ]] || error "PULSE_DEB_DIR '${PULSE_DEB_DIR}' is not a directory."
        SDK_DEBS="${PULSE_DEB_DIR}"
        info "Using Pulse SDK .deb packages from ${SDK_DEBS}"
    elif compgen -G "${REPO_DIR}/sdk/debs/pexninja_*.deb" > /dev/null \
      || compgen -G "${REPO_DIR}/sdk/debs/libpexpulse_*.deb" > /dev/null; then
        SDK_DEBS="${REPO_DIR}/sdk/debs"
        info "Using Pulse SDK .deb packages from ${SDK_DEBS}"
    elif download_sdk_debs; then
        SDK_DEBS="${DOWNLOAD_DIR}"
    else
        info "Cloning doppler repository to get the Pulse SDK..."
        rm -rf "${DOPPLER_DIR}"
        git clone --depth 1 "${DOPPLER_REPO}" "${DOPPLER_DIR}"

        SDK_DEBS="${DOPPLER_DIR}/sdk/linux/debs"
        if [[ ! -d "${SDK_DEBS}" ]]; then
            error "Could not find sdk/linux/debs in the doppler repository. " \
                  "Check that the repo structure matches what is expected."
        fi
    fi

    info "Installing Pulse SDK .deb packages..."

    # Pick the packages that match this machine's architecture (a Raspberry Pi
    # running 64-bit Ubuntu is 'arm64').  Installing an 'amd64' package on
    # arm64 fails with "package architecture (amd64) does not match system
    # (arm64)".
    find_deb() {
        local pkg="$1" deb
        for deb in "${SDK_DEBS}/${pkg}"_*_"${DPKG_ARCH}".deb \
                   "${SDK_DEBS}/${pkg}"_*_all.deb; do
            [[ -f "${deb}" ]] || continue
            printf '%s\n' "${deb}"
            return 0
        done
        return 1
    }

    # Current SDK releases are a single 'pexninja' package; older releases
    # split the SDK into libpexcommon / libpexpulse / libpexpulse-dev.
    SDK_PACKAGES=()
    MISSING_PACKAGES=()
    if deb="$(find_deb pexninja)"; then
        SDK_PACKAGES+=("${deb}")
    else
        for pkg in libpexcommon libpexpulse libpexpulse-dev; do
            if deb="$(find_deb "${pkg}")"; then
                SDK_PACKAGES+=("${deb}")
            else
                MISSING_PACKAGES+=("${pkg}")
            fi
        done
    fi

    if (( ${#SDK_PACKAGES[@]} == 0 || ${#MISSING_PACKAGES[@]} )); then
        AVAILABLE="$(ls "${SDK_DEBS}" 2>/dev/null | tr '\n' ' ')"
        error "No '${DPKG_ARCH}' Pulse SDK packages found: expected either" \
              "pexninja_<version>_${DPKG_ARCH}.deb or ${MISSING_PACKAGES[*]}." \
              "Available files in ${SDK_DEBS}: ${AVAILABLE:-<none>}." \
              "Pexip publishes the Pulse SDK for amd64 only, so on ${DPKG_ARCH}" \
              "you must supply the packages yourself and point the installer at" \
              "them:  sudo PULSE_DEB_DIR=/path/to/debs bash scripts/install.sh"
    fi

    dpkg -i "${SDK_PACKAGES[@]}" || true
    apt-get install -f -y   # resolve any missing dependencies

    if ! SDK_PREFIX="$(detect_sdk_prefix)"; then
        error "The Pulse SDK packages did not install correctly —" \
              "include/pexpulse/pulse.h was not found under" \
              "${SDK_PREFIXES[*]}.  Check the dpkg output above for errors."
    fi

    rm -rf "${DOPPLER_DIR}" "${DOWNLOAD_DIR}"
    info "Pulse SDK installed."
fi

# ---------------------------------------------------------------------------
# 3. Build
# ---------------------------------------------------------------------------
info "Building previs-client..."
rm -rf "${BUILD_DIR}"
cmake -S "${REPO_DIR}" -B "${BUILD_DIR}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="${INSTALL_PREFIX}" \
    -DPEXIP_PREFIX="${SDK_PREFIX}"

cmake --build "${BUILD_DIR}" -j"$(nproc)"

# ---------------------------------------------------------------------------
# 4. Install binary and systemd service
# ---------------------------------------------------------------------------
info "Installing previs-client binary..."
install -m 755 "${BUILD_DIR}/previs-client" "${INSTALL_PREFIX}/bin/previs-client"

info "Installing config file..."
mkdir -p /etc/previs-client
if [[ -f /etc/previs-client/config.yaml ]]; then
    warning "/etc/previs-client/config.yaml already exists — not overwriting."
    warning "Your current config is preserved. New template saved as config.yaml.new"
    install -m 644 "${REPO_DIR}/config.yaml" /etc/previs-client/config.yaml.new
else
    install -m 644 "${REPO_DIR}/config.yaml" /etc/previs-client/config.yaml
fi

info "Installing systemd service..."
# The unit ships with default SDK paths; point it at wherever the SDK actually
# got installed on this machine (/opt/pexninja or /opt/pexip).
sed -e "s|PEX_BASE_PATH=[^\"]*|PEX_BASE_PATH=${SDK_PREFIX}|" \
    -e "s|LD_LIBRARY_PATH=[^\"]*|LD_LIBRARY_PATH=${SDK_PREFIX}/lib|" \
    "${REPO_DIR}/systemd/previs-client.service" \
    > /etc/systemd/system/previs-client.service
chmod 644 /etc/systemd/system/previs-client.service

# ---------------------------------------------------------------------------
# 5. System user
# ---------------------------------------------------------------------------
if ! id -u previs &>/dev/null; then
    info "Creating system user 'previs'..."
    useradd -r -s /usr/sbin/nologin -G audio,video -c "Previs video client" previs
else
    info "System user 'previs' already exists."
    # Ensure group memberships.
    usermod -aG audio,video previs 2>/dev/null || true
fi

# ---------------------------------------------------------------------------
# 6. Enable and start the service
# ---------------------------------------------------------------------------
info "Reloading systemd daemon..."
systemctl daemon-reload

info "Enabling previs-client service (starts on boot)..."
systemctl enable previs-client

info "Starting previs-client service..."
systemctl restart previs-client

# ---------------------------------------------------------------------------
# Done
# ---------------------------------------------------------------------------
echo ""
info "Installation complete!"
echo ""
echo "  Configuration file : /etc/previs-client/config.yaml"
echo "  Service status     : sudo systemctl status previs-client"
echo "  Live logs          : sudo journalctl -fu previs-client"
echo ""
warning "IMPORTANT: Edit /etc/previs-client/config.yaml to set your"
warning "           Pexip server, VMR and display name, then restart:"
echo ""
echo "      sudo nano /etc/previs-client/config.yaml"
echo "      sudo systemctl restart previs-client"
echo ""
