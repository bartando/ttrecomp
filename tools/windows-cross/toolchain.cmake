# The CRT and SDK paths come from INCLUDE/LIB (set in the Dockerfile), which
# clang and lld-link read the same way MSVC does. That keeps clang's own
# headers ahead of the UCRT ones, unlike -isystem.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR AMD64)

set(CMAKE_C_COMPILER clang-20)
set(CMAKE_CXX_COMPILER clang++-20)
set(CMAKE_C_COMPILER_TARGET x86_64-pc-windows-msvc)
set(CMAKE_CXX_COMPILER_TARGET x86_64-pc-windows-msvc)
set(CMAKE_RC_COMPILER llvm-rc-20)
set(CMAKE_AR llvm-ar-20)
set(CMAKE_RANLIB llvm-ranlib-20)
set(CMAKE_MT llvm-mt-20)

# Static CRT, so the zip runs without the VC++ Redistributable. Every
# dependency links statically, so the exe holds the only CRT copy. The policy
# default makes subprojects with an old cmake_minimum_required honor it too.
# xwin ships the release CRT only.
set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreaded)
set(CMAKE_TRY_COMPILE_CONFIGURATION Release)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
