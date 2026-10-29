# Clang cross toolchain for the Switch 2 (HBLauncher) port: Clang (upstream WiiCompiled requires
# it) targeting aarch64-linux-musl, using the patched musl sysroot, libstdc++/libgcc and GNU ld
# from HBLauncher's musl-cross-make toolchain (GCC 15.1.0, musl 1.2.6, binutils 2.44, every
# target library built with -mbranch-protection=bti). Static, non-PIE, BTI landing pads
# everywhere: console code pages are BTI-guarded.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(HBX_TOOLCHAIN "$ENV{HBXL_TOOLCHAIN}" CACHE PATH "Patched musl toolchain")
if(NOT EXISTS "${HBX_TOOLCHAIN}/bin/aarch64-linux-musl-ld")
  message(FATAL_ERROR "Set HBXL_TOOLCHAIN to the patched musl toolchain")
endif()
set(HBX_TRIPLE aarch64-linux-musl)
file(GLOB HBX_GCC_INSTALL_DIR LIST_DIRECTORIES true "${HBX_TOOLCHAIN}/lib/gcc/${HBX_TRIPLE}/*")

foreach(lang C CXX ASM)
  set(CMAKE_${lang}_COMPILER_TARGET ${HBX_TRIPLE})
endforeach()
set(CMAKE_C_COMPILER clang)
set(CMAKE_CXX_COMPILER clang++)
set(CMAKE_ASM_COMPILER clang)
set(CMAKE_AR "${HBX_TOOLCHAIN}/bin/${HBX_TRIPLE}-ar")
set(CMAKE_RANLIB "${HBX_TOOLCHAIN}/bin/${HBX_TRIPLE}-ranlib")
set(CMAKE_SYSROOT "${HBX_TOOLCHAIN}/${HBX_TRIPLE}")
set(CMAKE_FIND_ROOT_PATH "${CMAKE_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(CMAKE_POSITION_INDEPENDENT_CODE OFF)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

# The console build and the desktop test build compile identically (HBX_SWITCH2); only the final
# link differs: HBLauncher's runtime + linker script for the console, desktop stubs otherwise.
set(HBX_COMMON "--gcc-install-dir=${HBX_GCC_INSTALL_DIR} -mbranch-protection=bti -fno-pie -fno-pic -ffunction-sections -fdata-sections -DHBX_SWITCH2=1")
set(CMAKE_C_FLAGS_INIT "${HBX_COMMON}")
set(CMAKE_CXX_FLAGS_INIT "${HBX_COMMON}")
set(CMAKE_ASM_FLAGS_INIT "${HBX_COMMON}")
# GNU ld from the toolchain (HBLauncher's hbxl.ld is a GNU ld script). -z force-bti warns about
# any input object without the BTI property, so a missing landing pad shows up at link time.
# HBX_EXTRA_LINK appends the variant-specific part (desktop stubs, or HBLauncher's runtime and
# linker script); setting CMAKE_EXE_LINKER_FLAGS directly would replace these defaults.
set(HBX_EXTRA_LINK "" CACHE STRING "Extra executable link flags/objects for this variant")
set(CMAKE_EXE_LINKER_FLAGS_INIT "--ld-path=${HBX_TOOLCHAIN}/bin/${HBX_TRIPLE}-ld -static -no-pie -Wl,--gc-sections -Wl,-z,force-bti ${HBX_EXTRA_LINK}")
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
set(THREADS_PREFER_PTHREAD_FLAG ON)
set(ENV{PKG_CONFIG_LIBDIR} "${CMAKE_SYSROOT}/lib/pkgconfig")
set(ENV{PKG_CONFIG_PATH} "")
