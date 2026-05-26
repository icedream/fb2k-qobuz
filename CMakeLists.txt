cmake_minimum_required(VERSION 3.16)
project(foo_qobuz CXX RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---- Version detection from Git tags ----------------------------------------------
# Uses git describe --always --tags to derive version string, fallback to "dev" if not in a git repo.
# Format: v1.2.3 -> "1.2.3", v1.2.3-5-gABCDEF -> "1.2.3-5-gABCDEF"
find_package(Git QUIET)
if(GIT_FOUND AND EXISTS "${CMAKE_SOURCE_DIR}/.git")
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" describe --always --tags --long
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        OUTPUT_VARIABLE FOO_QOBUZ_VERSION
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    # Strip leading 'v' if present (v1.2.3 -> 1.2.3)
    string(REGEX REPLACE "^v" "" FOO_QOBUZ_VERSION "${FOO_QOBUZ_VERSION}")
else()
    set(FOO_QOBUZ_VERSION "dev")
endif()

message(STATUS "foo_qobuz version: ${FOO_QOBUZ_VERSION}")

# Parse version string for PE/VERSIONINFO metadata
# Input: "1.2.3-5-gABCDEF" or "1.2.3" or "dev"
# Output: VERSION_MAJOR, VERSION_MINOR, VERSION_PATCH, VERSION_BUILD
string(REGEX MATCH "^([0-9]+)\\.([0-9]+)\\.([0-9]+)" _ "${FOO_QOBUZ_VERSION}")
if(CMAKE_MATCH_0)
    set(VERSION_MAJOR "${CMAKE_MATCH_1}")
    set(VERSION_MINOR "${CMAKE_MATCH_2}")
    set(VERSION_PATCH "${CMAKE_MATCH_3}")
else()
    set(VERSION_MAJOR "0")
    set(VERSION_MINOR "0")
    set(VERSION_PATCH "0")
endif()

# Extract build number from "X-gHASH" suffix (number of commits since tag)
string(REGEX MATCH "-([0-9]+)-g" _ "${FOO_QOBUZ_VERSION}")
if(CMAKE_MATCH_1)
    set(VERSION_BUILD "${CMAKE_MATCH_1}")
else()
    set(VERSION_BUILD "0")
endif()

# Windows PE version format (comma-separated, 4 components for FileVersion/ProductVersion)
set(VERSION_PE "${VERSION_MAJOR},${VERSION_MINOR},${VERSION_PATCH},${VERSION_BUILD}")
message(STATUS "PE version: ${VERSION_PE}")

# ---- Architecture detection ----------------------------------------------
# CMAKE_SYSTEM_PROCESSOR is set by the toolchain file (x86_64 or x86).
# On a native Windows MSVC build, fall back to CMAKE_VS_PLATFORM_NAME or MSVC_CXX_ARCHITECTURE_ID.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(i[3-6]86|x86)$" OR
   CMAKE_VS_PLATFORM_NAME STREQUAL "Win32" OR
   MSVC_CXX_ARCHITECTURE_ID STREQUAL "X86")
    set(TARGET_ARCH_X86 TRUE)
else()
    set(TARGET_ARCH_X86 FALSE)
endif()

# ---- xwin auto-download --------------------------------------------------
# When cross-compiling on Linux, the MSVC headers and Windows SDK libs are
# provided by xwin. If the expected lib directory is absent, CMake can
# download and splat them automatically via the `xwin` CLI tool.
#
# Options:
#   XWIN_DIR             - Path where xwin has been splatted (default: ~/xwin).
#                          Also settable via -DXWIN_DIR= on the cmake command line.
#   XWIN_AUTO_DOWNLOAD   - Download missing xwin libs automatically (default: ON).
#   XWIN_ACCEPT_LICENSE  - Must be set to ON to allow auto-download; this implies
#                          acceptance of the Microsoft Visual C++ license distributed
#                          through xwin. See: https://visualstudio.microsoft.com/license-terms/vs2022-clt-build-tools/
#
if(CMAKE_CROSSCOMPILING AND CMAKE_SYSTEM_NAME STREQUAL "Windows")
    set(XWIN_DIR "${CMAKE_SOURCE_DIR}/.deps/xwin" CACHE PATH
        "Path to xwin splatted MSVC headers and Windows SDK libs")
    option(XWIN_AUTO_DOWNLOAD
        "Automatically download missing Windows SDK headers/libs via xwin" ON)
    option(XWIN_ACCEPT_LICENSE
        "Accept the Microsoft Visual C++ license required for xwin auto-download" OFF)

    if(TARGET_ARCH_X86)
        set(_XWIN_CHECK_ARCH "x86")
    else()
        set(_XWIN_CHECK_ARCH "x86_64")
    endif()

    if(XWIN_AUTO_DOWNLOAD AND NOT EXISTS "${XWIN_DIR}/crt/lib/${_XWIN_CHECK_ARCH}")
        if(NOT XWIN_ACCEPT_LICENSE)
            message(FATAL_ERROR
                "Windows SDK headers not found at XWIN_DIR=${XWIN_DIR} and "
                "XWIN_ACCEPT_LICENSE is OFF.\n"
                "To auto-download, review the Microsoft Visual C++ license at:\n"
                "  https://visualstudio.microsoft.com/license-terms/vs2022-clt-build-tools/\n"
                "then re-run CMake with -DXWIN_ACCEPT_LICENSE=ON.")
        endif()
        find_program(XWIN_EXECUTABLE xwin
            DOC "xwin tool for downloading and splatting MSVC headers")
        if(NOT XWIN_EXECUTABLE)
            message(FATAL_ERROR
                "xwin not found. Install it from https://github.com/Jake-Shadle/xwin "
                "or provide pre-splatted headers at XWIN_DIR=${XWIN_DIR}.")
        endif()
        message(STATUS "Downloading Windows SDK headers to ${XWIN_DIR} via xwin ...")
        # Always splat both arches together — xwin replaces the output dir per run,
        # so separate invocations would overwrite each other.
        execute_process(
            COMMAND "${XWIN_EXECUTABLE}"
                --accept-license
                --arch x86 --arch x86_64
                --cache-dir "${CMAKE_SOURCE_DIR}/.deps/xwin-cache"
                splat --output "${XWIN_DIR}"
            RESULT_VARIABLE _XWIN_RESULT
        )
        if(NOT _XWIN_RESULT EQUAL 0)
            message(FATAL_ERROR "xwin exited with code ${_XWIN_RESULT}")
        endif()
        message(STATUS "xwin download complete.")
    endif()
endif()

# ---- Compile definitions -------------------------------------------------
add_compile_definitions(
    WIN32
    _WIN32
    _WIN32_WINNT=0x0600
    WINVER=0x0600
    # WIN32_LEAN_AND_MEAN intentionally omitted: fb2k SDK uses COM `interface` keyword
    # which requires objbase.h pulled in via the full windows.h
    NOMINMAX
    UNICODE
    _UNICODE
    FOOBAR2000_HAVE_CFG_VAR_LEGACY=1
    FOOBAR2000_TARGET_VERSION=82
    _CRT_SECURE_NO_WARNINGS
    _CRT_NONSTDC_NO_WARNINGS
)
if(NOT TARGET_ARCH_X86)
    add_compile_definitions(WIN64 _WIN64)
endif()

# ---- Compiler detection --------------------------------------------------
# IS_MSVC_LIKE: native MSVC *or* clang-cl (both use MSVC ABI / flags)
if(MSVC)
    set(IS_MSVC_LIKE TRUE)
endif()
# IS_CLANG_CL: specifically clang-cl (supports extra -W flags that MSVC ignores)
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC")
    set(IS_CLANG_CL TRUE)
endif()

# ---- foobar2000 SDK ------------------------------------------------------
# Options:
#   FB2K_SDK_DIR      - Path to an already-extracted SDK (skips download).
#   FB2K_SDK_VERSION  - Version date string (e.g. "2025-03-07") or "latest".
#                       Ignored when FB2K_SDK_DIR is set. Default: "latest".
#
# Downloaded SDKs are cached under <source-root>/.deps/fb2k-sdk/ so they
# survive build-directory deletion. .deps/ is excluded via .gitignore.
set(FB2K_SDK_DIR "" CACHE PATH
    "Path to an existing foobar2000 SDK directory (skips auto-download)")
set(FB2K_SDK_VERSION "latest" CACHE STRING
    "foobar2000 SDK version to download: date string (YYYY-MM-DD) or \"latest\"")

if(FB2K_SDK_DIR AND EXISTS "${FB2K_SDK_DIR}/pfc")
    set(SDK_DIR "${FB2K_SDK_DIR}")
    message(STATUS "foobar2000 SDK: using user-provided ${SDK_DIR}")
else()
    set(_FB2K_DEPS_DIR "${CMAKE_SOURCE_DIR}/.deps/fb2k-sdk")

    # Resolve "latest" by scraping the SDK download page.
    if(FB2K_SDK_VERSION STREQUAL "latest")
        set(_FB2K_SDK_PAGE "${_FB2K_DEPS_DIR}/_sdk_page.html")
        file(DOWNLOAD "https://www.foobar2000.org/SDK" "${_FB2K_SDK_PAGE}"
            TIMEOUT 30 STATUS _dl_status)
        list(GET _dl_status 0 _dl_code)
        if(NOT _dl_code EQUAL 0)
            message(FATAL_ERROR "Could not fetch foobar2000 SDK page: ${_dl_status}")
        endif()
        file(READ "${_FB2K_SDK_PAGE}" _sdk_page_content)
        string(REGEX MATCH "SDK-([0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9])\\.7z" _ "${_sdk_page_content}")
        if(NOT CMAKE_MATCH_1)
            message(FATAL_ERROR "Could not parse latest foobar2000 SDK version from download page")
        endif()
        set(_resolved_version "${CMAKE_MATCH_1}")
    else()
        set(_resolved_version "${FB2K_SDK_VERSION}")
    endif()

    set(_FB2K_SDK_EXTRACT_DIR "${_FB2K_DEPS_DIR}/SDK-${_resolved_version}")

    if(NOT EXISTS "${_FB2K_SDK_EXTRACT_DIR}/pfc")
        set(_sdk_url "https://www.foobar2000.org/downloads/SDK-${_resolved_version}.7z")
        set(_sdk_archive "${_FB2K_DEPS_DIR}/SDK-${_resolved_version}.7z")
        message(STATUS "Downloading foobar2000 SDK ${_resolved_version} from ${_sdk_url} ...")
        file(MAKE_DIRECTORY "${_FB2K_SDK_EXTRACT_DIR}")
        file(DOWNLOAD "${_sdk_url}" "${_sdk_archive}"
            SHOW_PROGRESS TIMEOUT 120 STATUS _dl_status)
        list(GET _dl_status 0 _dl_code)
        if(NOT _dl_code EQUAL 0)
            file(REMOVE_RECURSE "${_FB2K_SDK_EXTRACT_DIR}")
            message(FATAL_ERROR "Failed to download foobar2000 SDK: ${_dl_status}")
        endif()
        execute_process(
            COMMAND "${CMAKE_COMMAND}" -E tar xf "${_sdk_archive}"
            WORKING_DIRECTORY "${_FB2K_SDK_EXTRACT_DIR}"
            RESULT_VARIABLE _extract_result
        )
        if(NOT _extract_result EQUAL 0)
            file(REMOVE_RECURSE "${_FB2K_SDK_EXTRACT_DIR}")
            message(FATAL_ERROR "Failed to extract foobar2000 SDK archive (exit ${_extract_result})")
        endif()
        file(REMOVE "${_sdk_archive}")
    endif()

    if(NOT EXISTS "${_FB2K_SDK_EXTRACT_DIR}/pfc")
        message(FATAL_ERROR "Unexpected SDK layout after extraction in ${_FB2K_SDK_EXTRACT_DIR}")
    endif()

    set(SDK_DIR "${_FB2K_SDK_EXTRACT_DIR}")
    message(STATUS "foobar2000 SDK: ${_resolved_version} at ${SDK_DIR}")
endif()

# pfc sources (all compile safely under _WIN32)
# pfc-fb2k-hooks.cpp provides stub crash/error hooks for standalone use;
# when building a fb2k component, foobar2000/SDK/utility.cpp already provides
# them — include both causes duplicate symbol errors at link time.
file(GLOB SDK_PFC_SRCS  "${SDK_DIR}/pfc/*.cpp")
list(FILTER SDK_PFC_SRCS EXCLUDE REGEX ".*pfc-fb2k-hooks\\.cpp$")

# fb2k SDK sources (exclude Apple .mm files)
file(GLOB SDK_FB2K_ALL  "${SDK_DIR}/foobar2000/SDK/*.cpp")
list(FILTER SDK_FB2K_ALL EXCLUDE REGEX ".*\\.mm$")

set(SDK_CLIENT_SRC
    "${SDK_DIR}/foobar2000/foobar2000_component_client/component_client.cpp")

# ---- Configure RC file with version variables --------------------------------
# The RC file uses @VERSION_*@ placeholders which get substituted here
configure_file(
    "${CMAKE_SOURCE_DIR}/src/foo_qobuz.rc.in"
    "${CMAKE_BINARY_DIR}/foo_qobuz.rc"
    @ONLY
)

# ---- Plugin sources ------------------------------------------------------
set(PLUGIN_SRCS
    src/main.cpp
    src/qobuz_api.cpp
    src/qobuz_bundle.cpp
    src/qobuz_input.cpp
    src/qobuz_playlist_loader.cpp
    src/qobuz_prefs.cpp
    src/qobuz_search.cpp
    "${CMAKE_BINARY_DIR}/foo_qobuz.rc"
)

add_library(foo_qobuz SHARED
    ${SDK_PFC_SRCS}
    ${SDK_FB2K_ALL}
    ${SDK_CLIENT_SRC}
    ${PLUGIN_SRCS}
)

# ---- Include directories -------------------------------------------------
# On Linux cross-compile the file-system is case-sensitive; case-shim dirs with
# symlinks to Windows SDK headers are added via /imsvc in the toolchain file.
# On native Windows the FS is case-insensitive so no shims are needed here.
target_include_directories(foo_qobuz PRIVATE
    ${SDK_DIR}                          # for <pfc/...>
    ${SDK_DIR}/foobar2000               # for <SDK/...> and <helpers/...>
    ${CMAKE_SOURCE_DIR}/src             # for resource.h, stdafx.h
    ${CMAKE_SOURCE_DIR}/third_party     # for <nlohmann/json.hpp>
)

# ---- Compiler flags ------------------------------------------------------
# Set default build type for single-config generators (e.g., Unix Makefiles, Ninja)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type" FORCE)
endif()

if(IS_MSVC_LIKE)
    # Common MSVC flags
    set(_MSVC_COMMON "/W1 /wd4005 /wd4996")
    
    # Build-type-specific flags
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        set(_MSVC_OPT "/Zi")
    else()
        set(_MSVC_OPT "/O2")
    endif()
    
    # Suppress C4005 (macro redefinition) - SDK headers redefine FOOBAR2000_*
    # macros that are also defined via add_compile_definitions() in CMakeLists.txt.
    # This is benign; we accept the SDK's definitions at include time.
    target_compile_options(foo_qobuz PRIVATE ${_MSVC_OPT} ${_MSVC_COMMON})
    if(IS_CLANG_CL)
        # clang-cl accepts GCC-style warning suppression flags; native MSVC does not
        target_compile_options(foo_qobuz PRIVATE
            -Wno-ignored-pragma-intrinsic
            -Wno-unknown-pragmas
            -Wno-unused-parameter
            -Wno-microsoft-cast
        )
    endif()
else()
    # MinGW / GCC cross-compile
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        set(_GCC_OPT "-g")
    else()
        set(_GCC_OPT "-O2")
    endif()
    
    target_compile_options(foo_qobuz PRIVATE
        ${_GCC_OPT}
        -Wall
        -Wno-unknown-pragmas
        -Wno-unused-parameter
        -Wno-missing-field-initializers
        -Wno-register
    )
endif()

# ---- Per-file flags (audio_math.cpp uses AVX/SSE intrinsics) -------------
if(IS_MSVC_LIKE)
    set_source_files_properties(${SDK_DIR}/pfc/audio_math.cpp PROPERTIES
        COMPILE_FLAGS "/arch:AVX")
else()
    if(TARGET_ARCH_X86)
        set_source_files_properties(${SDK_DIR}/pfc/audio_math.cpp PROPERTIES
            COMPILE_FLAGS "-mavx -msse4.1 -mssse3 -msse2")
    else()
        set_source_files_properties(${SDK_DIR}/pfc/audio_math.cpp PROPERTIES
            COMPILE_FLAGS "-mavx -msse4.1 -mssse3")
    endif()
endif()

# ---- Link libraries ------------------------------------------------------
if(IS_MSVC_LIKE)
    if(TARGET_ARCH_X86)
        set(_FB2K_SHARED_LIB "${SDK_DIR}/foobar2000/shared/shared-Win32.lib")
    else()
        set(_FB2K_SHARED_LIB "${SDK_DIR}/foobar2000/shared/shared-x64.lib")
    endif()
    target_link_libraries(foo_qobuz PRIVATE
        wininet.lib winhttp.lib comctl32.lib comdlg32.lib shell32.lib ole32.lib
        uuid.lib shlwapi.lib winmm.lib
        # shared.dll import library — provides SHARED_EXPORT functions (uCharLower, etc.)
        "${_FB2K_SHARED_LIB}"
    )
    target_link_options(foo_qobuz PRIVATE /DLL)
else()
    target_link_libraries(foo_qobuz PRIVATE
        wininet comctl32 comdlg32 shell32 ole32 uuid shlwapi winmm
    )
    target_link_options(foo_qobuz PRIVATE
        -static-libgcc -static-libstdc++ -static -lwinpthread
    )
endif()

# Output name must match VALIDATE_COMPONENT_FILENAME
set_target_properties(foo_qobuz PROPERTIES
    OUTPUT_NAME "foo_qobuz"
    PREFIX      ""
    SUFFIX      ".dll"
)

# ---- .fb2k-component packaging -------------------------------------------
# A .fb2k-component is a ZIP archive with:
#   foo_qobuz.dll        <- 32-bit (Win32) DLL at the archive root
#   x64/foo_qobuz.dll   <- 64-bit DLL under x64/
#
# Build both architectures first, then run one of them with:
#   cmake --build <build-dir> --target foo_qobuz_component \
#         -DFB2K_COMPONENT_EXTRA_DLL=<path-to-other-arch-dll>
#
# Options:
#   FB2K_COMPONENT_EXTRA_DLL - Path to the other-architecture DLL.
#                              For a 64-bit build, provide the 32-bit DLL here.
#                              For a 32-bit build, provide the 64-bit DLL here.
#                              If omitted, the package contains only one arch.
set(FB2K_COMPONENT_EXTRA_DLL "" CACHE FILEPATH
    "Path to the other-architecture foo_qobuz.dll to bundle in the .fb2k-component")

add_custom_target(foo_qobuz_component
    COMMAND "${CMAKE_COMMAND}"
        "-DFOO_DLL=$<TARGET_FILE:foo_qobuz>"
        "-DOUTPUT=${CMAKE_BINARY_DIR}/foo_qobuz.fb2k-component"
        "-DARCH_X86=${TARGET_ARCH_X86}"
        "-DEXTRA_DLL=${FB2K_COMPONENT_EXTRA_DLL}"
        -P "${CMAKE_SOURCE_DIR}/cmake/package_component.cmake"
    DEPENDS foo_qobuz
    COMMENT "Packaging foo_qobuz.fb2k-component"
    VERBATIM
)
