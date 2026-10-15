# SDL3 dependency.
#
# Default: build SDL 3.2.x from the pinned release tarball as a static library,
# so the same SDL ships on every target (Raspberry Pi OS Bookworm has no SDL3
# package). Linux backends (X11, Wayland, KMSDRM, ALSA, PulseAudio,
# PipeWire) are dlopen()ed at runtime, so the binary runs on desktops and on
# a console-only Pi alike — as long as their dev headers were present at
# build time (see README).
#
# -DNEOSMK_SYSTEM_SDL3=ON uses an installed SDL3 (find_package) instead.
# -DNEOSMK_SDL3_SOURCE=<path> builds from a local SDL source tree/tarball.

option(NEOSMK_SYSTEM_SDL3 "Use an installed SDL3 instead of building the pinned release" OFF)
set(NEOSMK_SDL3_VERSION "3.2.30")
set(NEOSMK_SDL3_SHA256 "4c3b09330d866dc52eb65b66259a6684ad387252ca8c57901b3a2b534eb42e3d")
set(NEOSMK_SDL3_SOURCE "" CACHE PATH "Local SDL3 source directory or tarball (offline builds)")

if(NEOSMK_SYSTEM_SDL3)
    find_package(SDL3 3.2 REQUIRED CONFIG)
    if(NOT TARGET SDL3::SDL3)
        message(FATAL_ERROR "SDL3::SDL3 target not found")
    endif()
    message(STATUS "NeoSMK: using system SDL3 ${SDL3_VERSION}")
    return()
endif()

include(FetchContent)

set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
set(SDL_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
# Subsystems the game never touches.
set(SDL_CAMERA OFF CACHE BOOL "" FORCE)
set(SDL_GPU OFF CACHE BOOL "" FORCE)
set(SDL_RENDER OFF CACHE BOOL "" FORCE)
set(SDL_VULKAN OFF CACHE BOOL "" FORCE)
set(SDL_DIALOG OFF CACHE BOOL "" FORCE)
set(SDL_TRAY OFF CACHE BOOL "" FORCE)
set(SDL_SENSOR OFF CACHE BOOL "" FORCE)

if(NEOSMK_SDL3_SOURCE)
    if(IS_DIRECTORY "${NEOSMK_SDL3_SOURCE}")
        FetchContent_Declare(SDL3 SOURCE_DIR "${NEOSMK_SDL3_SOURCE}")
    else()
        FetchContent_Declare(SDL3 URL "${NEOSMK_SDL3_SOURCE}" URL_HASH SHA256=${NEOSMK_SDL3_SHA256})
    endif()
else()
    FetchContent_Declare(SDL3
        URL "https://github.com/libsdl-org/SDL/releases/download/release-${NEOSMK_SDL3_VERSION}/SDL3-${NEOSMK_SDL3_VERSION}.tar.gz"
        URL_HASH SHA256=${NEOSMK_SDL3_SHA256}
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
endif()
FetchContent_MakeAvailable(SDL3)
message(STATUS "NeoSMK: building SDL ${NEOSMK_SDL3_VERSION} (static)")
