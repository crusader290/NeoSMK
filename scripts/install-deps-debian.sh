#!/bin/sh
# Install NeoSMK build dependencies on Debian / Ubuntu / Raspberry Pi OS.
#
#   scripts/install-deps-debian.sh          native build (x86-64 or a Pi)
#   scripts/install-deps-debian.sh arm64    cross-compile for the Pi 4 from x86-64
#
# SDL3 is built from source by CMake; these are the headers it needs so its
# video/audio backends (X11, Wayland, KMS/DRM, ALSA, PulseAudio, PipeWire)
# get compiled in. They are dlopen()ed at runtime, so the resulting binary
# also runs on systems that lack some of them.
set -eu

ARCH="${1:-}"
SUFFIX=""
TOOLS="build-essential cmake ninja-build pkg-config git ca-certificates"
if [ "$ARCH" = "arm64" ]; then
    dpkg --add-architecture arm64
    SUFFIX=":arm64"
    TOOLS="$TOOLS crossbuild-essential-arm64"
fi

LIBS="libasound2-dev libpulse-dev libx11-dev libxext-dev libxrandr-dev
      libxcursor-dev libxfixes-dev libxi-dev libxss-dev libxkbcommon-dev
      libwayland-dev libdrm-dev libgbm-dev libegl-dev libgles-dev libgl-dev
      libudev-dev libdbus-1-dev"
if apt-cache show libpipewire-0.3-dev >/dev/null 2>&1; then
    LIBS="$LIBS libpipewire-0.3-dev"
fi

PKGS="$TOOLS"
for p in $LIBS; do PKGS="$PKGS $p$SUFFIX"; done
# wayland-scanner runs on the build host
[ -n "$SUFFIX" ] && PKGS="$PKGS libwayland-bin"

apt-get update
DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends $PKGS
