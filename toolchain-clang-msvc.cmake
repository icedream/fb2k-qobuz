# Clang-cl cross-compilation toolchain targeting x86_64-pc-windows-msvc
# Requires: clang-cl, lld-link, xwin headers at ~/xwin
#
# Usage (from repo root):
#   cmake -B build -DCMAKE_TOOLCHAIN_FILE=toolchain-clang-msvc.cmake
#
# Override xwin location:
#   cmake -B build -DCMAKE_TOOLCHAIN_FILE=toolchain-clang-msvc.cmake -DXWIN_DIR=/path/to/xwin
#
# On Windows, no toolchain file is needed; just open the folder in Visual Studio
# or run: cmake -B build && cmake --build build

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

if(NOT DEFINED XWIN_DIR OR XWIN_DIR STREQUAL "")
    set(XWIN_DIR "${_REPO_DIR}/.deps/xwin")
endif()

set(CMAKE_C_COMPILER   clang-cl)
set(CMAKE_CXX_COMPILER clang-cl)
set(CMAKE_RC_COMPILER  llvm-rc)
set(CMAKE_LINKER       lld-link)
set(CMAKE_MT           llvm-mt)

set(CMAKE_C_COMPILER_TARGET   x86_64-pc-windows-msvc)
set(CMAKE_CXX_COMPILER_TARGET x86_64-pc-windows-msvc)

# Derive repo root from this toolchain file's location
get_filename_component(_REPO_DIR "${CMAKE_CURRENT_LIST_FILE}" DIRECTORY)

# Force release-mode MSVC runtime (/MD instead of /MDd) since xwin only has msvcrt.lib
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")

# Point clang-cl at the xwin MSVC + Windows SDK headers, plus our case-shim dir
set(MSVC_INCLUDE
    "${XWIN_DIR}/crt/include"
    "${XWIN_DIR}/sdk/include/ucrt"
    "${XWIN_DIR}/sdk/include/um"
    "${XWIN_DIR}/sdk/include/shared"
    "${_REPO_DIR}/compat-include-msvc"  # case-shim symlinks for mixed-case Windows headers
)

# Build /imsvc flags (MSVC-style system include, suppresses warnings from SDK headers)
foreach(dir ${MSVC_INCLUDE})
    string(APPEND _IMSVC_FLAGS " /imsvc \"${dir}\"")
endforeach()

# Force-include pfc_win_extras.h before every TU so that:
#   - WinSock2.h is included before windows.h (prevents winsock.h redefinition)
#   - windows.h is included without WIN32_LEAN_AND_MEAN (pulls in COM `interface` + timeapi)
#   - timeGetTime is declared before pfc/timers.h uses it
# Using absolute path so the CMake compiler-test TUs can also find it.
set(_FI_FLAG " /FI\"${_REPO_DIR}/compat-include-msvc/pfc_win_extras.h\"")

# Suppress macro redefinition warnings: SDK headers redefine FOOBAR2000_*
# macros that are also defined via add_compile_definitions() in CMakeLists.txt.
# This is benign; we accept the SDK's definitions at include time.
# C4005 = macro redefinition (MSVC warning code for clang-cl)
set(_MACRO_REDEF_SUPPRESSION " /wd4005")

set(CMAKE_C_FLAGS_INIT   "${_IMSVC_FLAGS}${_FI_FLAG}${_MACRO_REDEF_SUPPRESSION}")
set(CMAKE_CXX_FLAGS_INIT "${_IMSVC_FLAGS}${_FI_FLAG}${_MACRO_REDEF_SUPPRESSION}")
set(CMAKE_RC_FLAGS_INIT  "${_MACRO_REDEF_SUPPRESSION}")

# Library paths for xwin
set(MSVC_LIB_DIR  "${XWIN_DIR}/crt/lib/x86_64")
set(WIN_SDK_UCRT  "${XWIN_DIR}/sdk/lib/ucrt/x86_64")
set(WIN_SDK_UM    "${XWIN_DIR}/sdk/lib/um/x86_64")

set(CMAKE_EXE_LINKER_FLAGS_INIT
    "/libpath:\"${MSVC_LIB_DIR}\" /libpath:\"${WIN_SDK_UCRT}\" /libpath:\"${WIN_SDK_UM}\"")
set(CMAKE_SHARED_LINKER_FLAGS_INIT
    "/libpath:\"${MSVC_LIB_DIR}\" /libpath:\"${WIN_SDK_UCRT}\" /libpath:\"${WIN_SDK_UM}\"")

set(CMAKE_FIND_ROOT_PATH "${XWIN_DIR}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
