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
#    2. Downloads and installs the Pexip Pulse SDK from the doppler repository.
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
BUILD_DIR="/tmp/previs-client-build"
INSTALL_PREFIX="/usr/local"
SDK_PREFIX="/opt/pexip"

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
    build-essential cmake git \
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
# 2. Pexip Pulse SDK (.deb packages from the doppler repo)
# ---------------------------------------------------------------------------
if [[ -f "${SDK_PREFIX}/include/pexpulse/pulse.h" ]]; then
    info "Pulse SDK already installed at ${SDK_PREFIX} — skipping."
else
    info "Cloning doppler repository to get the Pulse SDK..."
    rm -rf "${DOPPLER_DIR}"
    git clone --depth 1 "${DOPPLER_REPO}" "${DOPPLER_DIR}"

    SDK_DEBS="${DOPPLER_DIR}/sdk/linux/debs"
    if [[ ! -d "${SDK_DEBS}" ]]; then
        error "Could not find sdk/linux/debs in the doppler repository. " \
              "Check that the repo structure matches what is expected."
    fi

    info "Installing Pulse SDK .deb packages..."
    DPKG_ARCH="$(dpkg --print-architecture)"

    # The doppler repository ships packages for several architectures; pick the
    # ones that match this machine (a Raspberry Pi running 64-bit Ubuntu is
    # 'arm64').  Installing an 'amd64' package on arm64 fails with
    # "package architecture (amd64) does not match system (arm64)".
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

    SDK_PACKAGES=()
    MISSING_PACKAGES=()
    for pkg in libpexcommon libpexpulse libpexpulse-dev; do
        if deb="$(find_deb "${pkg}")"; then
            SDK_PACKAGES+=("${deb}")
        else
            MISSING_PACKAGES+=("${pkg}")
        fi
    done

    if (( ${#MISSING_PACKAGES[@]} )); then
        AVAILABLE="$(ls "${SDK_DEBS}" 2>/dev/null | tr '\n' ' ')"
        error "The Pulse SDK does not provide '${DPKG_ARCH}' packages for:" \
              "${MISSING_PACKAGES[*]}." \
              "Available files in ${SDK_DEBS}: ${AVAILABLE:-<none>}." \
              "The Pexip Pulse SDK must be built or obtained for ${DPKG_ARCH}" \
              "before this client can be installed on this machine."
    fi

    dpkg -i "${SDK_PACKAGES[@]}" || true
    apt-get install -f -y   # resolve any missing dependencies

    if [[ ! -f "${SDK_PREFIX}/include/pexpulse/pulse.h" ]]; then
        error "The Pulse SDK packages did not install correctly —" \
              "${SDK_PREFIX}/include/pexpulse/pulse.h is missing." \
              "Check the dpkg output above for errors."
    fi

    rm -rf "${DOPPLER_DIR}"
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
install -m 644 "${REPO_DIR}/systemd/previs-client.service" \
    /etc/systemd/system/previs-client.service

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
