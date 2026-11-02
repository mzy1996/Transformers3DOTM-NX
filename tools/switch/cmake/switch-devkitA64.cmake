# Nintendo Switch / libnx cross toolchain for devkitPro devkitA64.
#
# Based on skate3recomp-nx's toolchain, which builds a ReXGlue title for Switch.
# Differences, and why:
#   - The SDK's own CMake handles the Switch specifics once REXGLUE_PLATFORM_SWITCH
#     is set (switch_compat headers, static runtime, NVK, NRO packaging); this
#     file only picks the compiler, flags and linker specs.
#   - The static NVK driver comes from the Mesa Horizon SDK
#     (REXGLUE_SWITCH_NVK_SDK).
#
# Usage:
#   cmake -G Ninja -DCMAKE_TOOLCHAIN_FILE=<this file> \
#         -DREXGLUE_SWITCH_NVK_SDK=<mesa-...-horizon-sdk>/opt/devkitpro/portlibs/switch ...

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_VERSION 1)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(REXGLUE_PLATFORM_SWITCH ON CACHE BOOL "Build rexglue for Nintendo Switch/libnx")
set(REXGLUE_ENABLE_TRACY OFF CACHE BOOL "Tracy is unavailable on Switch" FORCE)
set(REXGLUE_ENABLE_PERF_COUNTERS OFF CACHE BOOL "No perf counters on Switch" FORCE)
set(REXGLUE_USE_VULKAN ON CACHE BOOL "NVK" FORCE)
set(REXGLUE_USE_D3D12 OFF CACHE BOOL "No D3D12 on Switch" FORCE)

if(NOT DEFINED DEVKITPRO)
    if(DEFINED ENV{DEVKITPRO})
        set(DEVKITPRO "$ENV{DEVKITPRO}" CACHE PATH "devkitPro root")
    elseif(CMAKE_HOST_WIN32 AND EXISTS "C:/devkitPro")
        set(DEVKITPRO "C:/devkitPro" CACHE PATH "devkitPro root")
    else()
        set(DEVKITPRO "/opt/devkitpro" CACHE PATH "devkitPro root")
    endif()
endif()

set(_DEVKITA64 "${DEVKITPRO}/devkitA64")
set(_DEVKITA64_BIN "${_DEVKITA64}/bin")
set(_DEVKITA64_PREFIX "aarch64-none-elf-")
if(CMAKE_HOST_WIN32)
    set(_DEVKITA64_SUFFIX ".exe")
else()
    set(_DEVKITA64_SUFFIX "")
endif()

set(CMAKE_C_COMPILER "${_DEVKITA64_BIN}/${_DEVKITA64_PREFIX}gcc${_DEVKITA64_SUFFIX}" CACHE FILEPATH "" FORCE)
set(CMAKE_CXX_COMPILER "${_DEVKITA64_BIN}/${_DEVKITA64_PREFIX}g++${_DEVKITA64_SUFFIX}" CACHE FILEPATH "" FORCE)
set(CMAKE_ASM_COMPILER "${_DEVKITA64_BIN}/${_DEVKITA64_PREFIX}gcc${_DEVKITA64_SUFFIX}" CACHE FILEPATH "" FORCE)
set(CMAKE_AR "${_DEVKITA64_BIN}/${_DEVKITA64_PREFIX}gcc-ar${_DEVKITA64_SUFFIX}" CACHE FILEPATH "" FORCE)
set(CMAKE_RANLIB "${_DEVKITA64_BIN}/${_DEVKITA64_PREFIX}gcc-ranlib${_DEVKITA64_SUFFIX}" CACHE FILEPATH "" FORCE)
set(CMAKE_NM "${_DEVKITA64_BIN}/${_DEVKITA64_PREFIX}nm${_DEVKITA64_SUFFIX}" CACHE FILEPATH "" FORCE)
set(CMAKE_OBJCOPY "${_DEVKITA64_BIN}/${_DEVKITA64_PREFIX}objcopy${_DEVKITA64_SUFFIX}" CACHE FILEPATH "" FORCE)
set(CMAKE_STRIP "${_DEVKITA64_BIN}/${_DEVKITA64_PREFIX}strip${_DEVKITA64_SUFFIX}" CACHE FILEPATH "" FORCE)
set(CMAKE_C_COMPILER_AR "${CMAKE_AR}" CACHE FILEPATH "" FORCE)
set(CMAKE_C_COMPILER_RANLIB "${CMAKE_RANLIB}" CACHE FILEPATH "" FORCE)
set(CMAKE_CXX_COMPILER_AR "${CMAKE_AR}" CACHE FILEPATH "" FORCE)
set(CMAKE_CXX_COMPILER_RANLIB "${CMAKE_RANLIB}" CACHE FILEPATH "" FORCE)

# Tegra X1 / libnx ABI. Local-exec TLS avoids the AArch64 linker relaxation
# corruption skate3recomp-nx observed in this very large static PIE on
# secondary threads.
set(_SWITCH_ARCH_FLAGS "-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -ftls-model=local-exec -fPIE -D__SWITCH__ -DNX -D_GNU_SOURCE -DSPDLOG_NO_TZ_OFFSET")
set(CMAKE_C_FLAGS_INIT "-ffunction-sections -fdata-sections ${_SWITCH_ARCH_FLAGS} -Wno-psabi")
set(CMAKE_CXX_FLAGS_INIT "-ffunction-sections -fdata-sections ${_SWITCH_ARCH_FLAGS} -Wno-psabi")
set(CMAKE_ASM_FLAGS_INIT "${_SWITCH_ARCH_FLAGS}")

# Disable target_precompile_headers() globally. Under the devkitPro MSYS cmake,
# generated PCH files embed MSYS-style absolute paths (/j/...) that the Windows
# GCC cannot open; PCH is only a compile-speed optimization here.
set(CMAKE_DISABLE_PRECOMPILE_HEADERS ON CACHE BOOL "Disable PCH (MSYS path issue)" FORCE)

# libnx's stock switch.specs resolves switch.ld through GCC's getenv spec
# function, which fails at the final link when DEVKITPRO is not exported in the
# shell (common on Windows). Write a build-local copy with the absolute path.
# On Windows the linker (ld.exe, a native binary) cannot open MSYS-style paths
# like /opt/devkitpro/... even though the compiler driver can, so substitute a
# Windows path when one is known to exist.
set(_SWITCH_SPECS_SOURCE "${DEVKITPRO}/libnx/switch.specs")
if(NOT EXISTS "${_SWITCH_SPECS_SOURCE}")
    message(FATAL_ERROR "libnx linker specs not found: ${_SWITCH_SPECS_SOURCE}")
endif()
if(EXISTS "C:/devkitPro/libnx/switch.ld")
    set(_SWITCH_SWITCH_LD "C:/devkitPro/libnx/switch.ld")
else()
    set(_SWITCH_SWITCH_LD "${DEVKITPRO}/libnx/switch.ld")
endif()
file(READ "${_SWITCH_SPECS_SOURCE}" _SWITCH_SPECS_CONTENT)
# Use the project's switch.ld (tools/switch/switch.ld): its .rodata PT_LOAD is
# marked Read|Write so libnx's RELR pass can write the ~600 jump-table/encoding
# relocations that land in .rodata. With libnx's stock script those targets sit
# in a read-only mapping and the RELR writes Data Abort before __libnx_app_init
# even runs (no logs). The script is copied into the build dir and referenced by
# a relative "-T switch.ld" because absolute paths contain spaces
# ("J:/XBOX360 port/...") which break the Windows GCC driver's argument split.
file(COPY "${CMAKE_CURRENT_LIST_DIR}/../switch.ld" DESTINATION "${CMAKE_BINARY_DIR}")
string(REPLACE
    "-T %:getenv(DEVKITPRO /libnx/switch.ld)"
    "-Tswitch.ld"
    _SWITCH_SPECS_CONTENT
    "${_SWITCH_SPECS_CONTENT}")
# LTO diag: the whole-program phase (-flto=2) emits dynamic relocations that
# land in read-only segments, tripping libnx's stock "-z text" hard error.
# Diagnostic builds relax it to "-z notext --warn-shared-textrel" so the link
# succeeds and the warning list pinpoints the offending sections; revert to
# plain "-z text" for the final release link.
set(_SWITCH_LTO_DIAG ON CACHE BOOL "Relax -z text to -z notext + warn-shared-textrel (LTO diagnostics)")
if(_SWITCH_LTO_DIAG)
    string(REPLACE "-z text" "-z notext --warn-shared-textrel" _SWITCH_SPECS_CONTENT "${_SWITCH_SPECS_CONTENT}")
endif()
set(_SWITCH_SPECS_CONFIGURED "${CMAKE_BINARY_DIR}/switch.specs")
file(WRITE "${_SWITCH_SPECS_CONFIGURED}" "${_SWITCH_SPECS_CONTENT}")
# --allow-multiple-definition: NVK's Rust static libraries each bundle the Rust
# runtime. Use a relative -specs path: Ninja runs with cwd=build dir, and an
# absolute path would contain spaces ("J:/XBOX360 port/...") that break the
# Windows GCC driver's argument splitting.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-specs=switch.specs -Wl,--gc-sections -Wl,--allow-multiple-definition -Wl,--no-relax")
set(CMAKE_DL_LIBS "")

include_directories(SYSTEM
    "${_DEVKITA64}/include"
    "${DEVKITPRO}/libnx/include"
    "${DEVKITPRO}/portlibs/switch/include")
link_directories(
    "${_DEVKITA64}/lib"
    "${DEVKITPRO}/libnx/lib"
    "${DEVKITPRO}/portlibs/switch/lib")

set(CMAKE_FIND_ROOT_PATH "${_DEVKITA64}" "${DEVKITPRO}/libnx" "${DEVKITPRO}/portlibs/switch")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
