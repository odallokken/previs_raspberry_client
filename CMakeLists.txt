cmake_minimum_required(VERSION 3.16)
project(previs-client
    VERSION 1.0.0
    DESCRIPTION "Headless Pexip Infinity video client for Raspberry Pi"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# Find the Pexip Pulse SDK
#
# Install via the .deb packages from the doppler repository:
#   sudo dpkg -i sdk/linux/debs/libpexcommon_*.deb \
#                sdk/linux/debs/libpexpulse_*.deb   \
#                sdk/linux/debs/libpexpulse-dev_*.deb
#   sudo apt-get install -f
#
# The packages install headers to /opt/pexip/include and the library to
# /opt/pexip/lib.  Override PEXIP_PREFIX if your SDK is elsewhere.
# ---------------------------------------------------------------------------
set(PEXIP_PREFIX "/opt/pexip" CACHE PATH "Root of the Pexip Pulse SDK install")

find_path(PEXIP_INCLUDE_DIR
    NAMES pexpulse/pulse.h
    HINTS "${PEXIP_PREFIX}/include"
    REQUIRED)

find_library(PEXIP_LIBRARY
    NAMES pexpulse
    HINTS "${PEXIP_PREFIX}/lib"
    REQUIRED)

add_library(pexip::pulse SHARED IMPORTED)
set_target_properties(pexip::pulse PROPERTIES
    IMPORTED_LOCATION "${PEXIP_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${PEXIP_INCLUDE_DIR}")

# ---------------------------------------------------------------------------
# Find yaml-cpp (sudo apt-get install libyaml-cpp-dev)
# ---------------------------------------------------------------------------
find_package(yaml-cpp REQUIRED)

# ---------------------------------------------------------------------------
# Main executable
# ---------------------------------------------------------------------------
add_executable(previs-client src/main.cpp)

target_link_libraries(previs-client PRIVATE
    pexip::pulse
    yaml-cpp
    Threads::Threads)

find_package(Threads REQUIRED)

# Set RPATH so the binary finds libpexpulse.so at runtime without needing
# LD_LIBRARY_PATH.
set_target_properties(previs-client PROPERTIES
    BUILD_RPATH "${PEXIP_PREFIX}/lib"
    INSTALL_RPATH "${PEXIP_PREFIX}/lib"
    INSTALL_RPATH_USE_LINK_PATH ON)

# ---------------------------------------------------------------------------
# Install targets
# ---------------------------------------------------------------------------
include(GNUInstallDirs)

install(TARGETS previs-client
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")

install(FILES config.yaml
    DESTINATION "/etc/previs-client"
    RENAME config.yaml)

install(FILES systemd/previs-client.service
    DESTINATION "/etc/systemd/system")
