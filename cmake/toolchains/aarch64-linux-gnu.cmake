# Cross-compile for 64-bit Raspberry Pi OS (Raspberry Pi 4, ARM64/AArch64)
# from an x86-64 Debian/Ubuntu host:
#   dpkg --add-architecture arm64
#   apt install crossbuild-essential-arm64 <dev packages>:arm64   (see README)
#   cmake -B build-pi4 -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/aarch64-linux-gnu.cmake -DNEOSMK_TUNE_PI4=ON
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(CMAKE_C_COMPILER   aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)

# Multiarch layout: arm64 libraries live in /usr/lib/aarch64-linux-gnu.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(CMAKE_FIND_ROOT_PATH /usr/aarch64-linux-gnu /usr)
set(CMAKE_LIBRARY_ARCHITECTURE aarch64-linux-gnu)

# pkg-config must report arm64 packages, not the host's.
set(ENV{PKG_CONFIG_LIBDIR} "/usr/lib/aarch64-linux-gnu/pkgconfig:/usr/share/pkgconfig")
set(ENV{PKG_CONFIG_PATH} "")
