cmake_minimum_required(VERSION 3.20)
project(NeoSMK VERSION 0.6.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(NEOSMK_TUNE_PI4 "Tune code generation for the Raspberry Pi 4 (Cortex-A72)" OFF)
option(NEOSMK_BUILD_TOOLS "Build the LakeSnes debug tools (oam_dumper, lakesnes_ref)" OFF)
option(NEOSMK_LTO "Link-time optimisation for Release builds" ON)

# Static runtime on MSVC so the Windows build is a single self-contained exe.
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

if(NEOSMK_TUNE_PI4)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
        add_compile_options(-mcpu=cortex-a72)
    else()
        message(WARNING "NEOSMK_TUNE_PI4 ignored: target processor is ${CMAKE_SYSTEM_PROCESSOR}")
    endif()
endif()

if(NEOSMK_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT NEOSMK_IPO_OK OUTPUT NEOSMK_IPO_MSG LANGUAGES C CXX)
endif()

# --- SDL3 (pinned release, static) ---
include(cmake/SDL3.cmake)

# --- snesrecomp: real SNES hardware backend (LakeSnes + SDL3/GL ES platform) ---
set(SNESRECOMP_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory(ext/snesrecomp)

# --- smk_game: recompiled game code ---
# An OBJECT library, not STATIC: the RECOMP_PATCH registrations are static
# constructors in objects nothing references by symbol, which GNU ld would
# drop from an archive.
add_library(smk_game OBJECT
    src/recomp/smk_boot.c
    src/recomp/smk_init.c
    src/recomp/smk_title.c
    src/recomp/smk_autogen.c
    src/neo/neo_world.c
    src/neo/neo_overlay.c
    src/neo/neo_gameplay.c
    src/neo/neo_itembox.c
    src/neo/neo_track.c
    src/neo/neo_pipes.c
    src/neo/neo_karts.c
    src/neo/neo_items.c
    src/neo/neo_minimap.c
)
target_include_directories(smk_game PUBLIC ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(smk_game PUBLIC snesrecomp)

# --- neosmk (executable) ---
add_executable(neosmk src/main/main.c)
target_link_libraries(neosmk PRIVATE smk_game snesrecomp)
target_compile_definitions(neosmk PRIVATE NEOSMK_VERSION="${PROJECT_VERSION}")
# snesrecomp pulls in C++ (ImGui menu) — link as C++ for the C++ runtime.
set_target_properties(neosmk PROPERTIES LINKER_LANGUAGE CXX)
if(NEOSMK_LTO AND NEOSMK_IPO_OK)
    foreach(t neosmk smk_game snesrecomp lakesnes_hw imgui)
        if(TARGET ${t})
            set_property(TARGET ${t} PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
        endif()
    endforeach()
endif()

if(NEOSMK_BUILD_TOOLS)
    # oam_dumper: runs the original ROM in LakeSnes, dumps OAM
    add_executable(oam_dumper tools/oam_dumper.c)
    target_include_directories(oam_dumper PRIVATE ext/snesrecomp/ext/LakeSnes)
    target_link_libraries(oam_dumper PRIVATE lakesnes_hw)
    # lakesnes_ref: ground-truth native snes_runFrame runner (SMKSNAP2 + BMP)
    add_executable(lakesnes_ref tools/lakesnes_ref.c)
    target_include_directories(lakesnes_ref PRIVATE ext/snesrecomp/ext/LakeSnes)
    target_link_libraries(lakesnes_ref PRIVATE lakesnes_hw)
endif()

install(TARGETS neosmk RUNTIME DESTINATION .)
install(FILES README.md LICENSE THIRD_PARTY_LICENSES.md DESTINATION .)
