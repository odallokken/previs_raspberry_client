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
# Recent SDK releases ship as a single 'pexninja' package that installs to
# /opt/pexninja (headers in include/, libpexpulse.so and its private siblings
# in lib/).  Older releases shipped libpexcommon / libpexpulse /
# libpexpulse-dev, which install to /opt/pexip.  Both layouts are searched;
# set PEXIP_PREFIX if your SDK lives somewhere else.
# ---------------------------------------------------------------------------
set(PEXIP_PREFIX "" CACHE PATH "Root of the Pexip Pulse SDK install")

set(PEXIP_SEARCH_PREFIXES "/opt/pexninja" "/opt/pexip")
if(PEXIP_PREFIX)
    list(INSERT PEXIP_SEARCH_PREFIXES 0 "${PEXIP_PREFIX}")
endif()

list(TRANSFORM PEXIP_SEARCH_PREFIXES APPEND "/include" OUTPUT_VARIABLE PEXIP_INCLUDE_HINTS)
list(TRANSFORM PEXIP_SEARCH_PREFIXES APPEND "/lib" OUTPUT_VARIABLE PEXIP_LIBRARY_HINTS)

find_path(PEXIP_INCLUDE_DIR
    NAMES pexpulse/pulse.h
    HINTS ${PEXIP_INCLUDE_HINTS}
    REQUIRED)

find_library(PEXIP_LIBRARY
    NAMES pexpulse
    HINTS ${PEXIP_LIBRARY_HINTS}
    REQUIRED)

get_filename_component(PEXIP_LIBRARY_DIR "${PEXIP_LIBRARY}" DIRECTORY)

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
# LD_LIBRARY_PATH.  '--disable-new-dtags' emits DT_RPATH instead of DT_RUNPATH:
# unlike RUNPATH, RPATH is also used when resolving the SDK's *own*
# dependencies (libpexlgpl.so, libonnxruntime.so.1, libamath.so, ...), which
# live in the same directory.  '-rpath-link' does the same at link time.
set_target_properties(previs-client PROPERTIES
    BUILD_RPATH "${PEXIP_LIBRARY_DIR}"
    INSTALL_RPATH "${PEXIP_LIBRARY_DIR}"
    INSTALL_RPATH_USE_LINK_PATH ON)

target_link_options(previs-client PRIVATE
    "-Wl,--disable-new-dtags"
    "-Wl,-rpath-link,${PEXIP_LIBRARY_DIR}")

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
