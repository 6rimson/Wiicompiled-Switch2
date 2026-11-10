# The game-independent native runtime (runtime/src). One definition serves
# three uses: the OBJECT library the products link from source, the STATIC
# archive the native prebuilt export ships (mkw_runtime), and that archive
# imported in place of compiling it (mkw_runtime_prebuilt).

include("${CMAKE_CURRENT_LIST_DIR}/RuntimeFingerprint.cmake")

if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|amd64|x86_64|X86_64)$")
    set(MKW_HOST_IS_X86_64 TRUE)
else()
    set(MKW_HOST_IS_X86_64 FALSE)
endif()

# Windows, Linux and Intel macOS x86_64 use the selected profile. AArch64
# builds are compiled locally for the host that will run them, so both Linux
# and Apple Silicon use the compiler's native CPU tuning rather than leaving
# target-specific performance on the table.
if(MKW_HOST_IS_X86_64)
    set(MKW_BASELINE_ARCH_FLAG -march=x86-64-${MKW_X86_CPU_PROFILE})
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
    # Cross builds name the target CPU instead (e.g. cortex-a78c for Switch 2).
    set(MKW_AARCH64_CPU "native" CACHE STRING "AArch64 -mcpu value; native tunes for the build machine")
    set(MKW_BASELINE_ARCH_FLAG -mcpu=${MKW_AARCH64_CPU})
else()
    set(MKW_BASELINE_ARCH_FLAG "")
endif()

if(MKW_PLATFORM_MACOS)
    find_library(MKW_COREAUDIO_FRAMEWORK CoreAudio REQUIRED)
endif()

# CMake decides WHOLE_ARCHIVE's form by probing ld.lld in ELF mode, which
# accepts --push-state; the MinGW lld driver rejects it at link time.
if(MKW_PLATFORM_WINDOWS)
    set(CMAKE_CXX_LINK_LIBRARY_USING_WHOLE_ARCHIVE
        "LINKER:--whole-archive" "<LINK_ITEM>" "LINKER:--no-whole-archive")
endif()

# Profile-guided optimization of the game code and the runtime. Aurora, Dawn and SDL are left out:
# a desktop training run never reaches their console paths, and PGO would build those as cold.
# MKW_PGO=generate builds a recording binary (it writes LLVM_PROFILE_FILE when it exits);
# MKW_PGO=use builds with MKW_PGO_PROFILE, the recordings merged by llvm-profdata.
set(MKW_PGO "" CACHE STRING "Profile-guided optimization: empty, generate or use")
set(MKW_PGO_PROFILE "" CACHE FILEPATH "Merged .profdata for MKW_PGO=use")
if(MKW_PGO STREQUAL "use" AND NOT EXISTS "${MKW_PGO_PROFILE}")
    message(FATAL_ERROR "MKW_PGO=use needs MKW_PGO_PROFILE set to a merged .profdata file")
elseif(NOT MKW_PGO STREQUAL "" AND NOT MKW_PGO STREQUAL "generate" AND NOT MKW_PGO STREQUAL "use")
    message(FATAL_ERROR "MKW_PGO must be empty, generate or use, not ${MKW_PGO}")
endif()

# Functions with internal linkage are named "<file>;<name>" in a profile; by default <file> is the
# full path, which holds the build directory. Naming them by the file name alone lets a profile
# recorded in the desktop build match the console build, on any machine (as KartPadNX does).
function(mkw_apply_pgo target)
    if(MKW_PGO STREQUAL "generate")
        target_compile_options(${target} PRIVATE -fprofile-generate
            "SHELL:-mllvm -static-func-full-module-prefix=false")
    elseif(MKW_PGO STREQUAL "use")
        target_compile_options(${target} PRIVATE "-fprofile-use=${MKW_PGO_PROFILE}"
            "SHELL:-mllvm -static-func-full-module-prefix=false"
            -Wno-profile-instr-unprofiled -Wno-profile-instr-out-of-date)
    endif()
endfunction()

# The executable link pulls in the profile runtime.
function(mkw_apply_pgo_link target)
    if(MKW_PGO STREQUAL "generate")
        target_link_options(${target} PRIVATE -fprofile-generate)
    endif()
endfunction()

function(mkw_apply_common_compile_options target)
    target_compile_options(${target} PRIVATE -O3 -ffast-math -w -pipe)
    mkw_apply_pgo(${target})
    if(MKW_PGO STREQUAL "generate")
        # Turns on src/pgo_profile_dump.cpp, which writes the profile when the run is stopped.
        target_compile_definitions(${target} PRIVATE MKW_PGO_GENERATE=1)
    endif()
endfunction()

function(mkw_apply_baseline_arch target)
    if(MKW_BASELINE_ARCH_FLAG)
        target_compile_options(${target} PRIVATE ${MKW_BASELINE_ARCH_FLAG})
        if(MKW_X86_CPU_PROFILE STREQUAL "v2")
            target_compile_definitions(${target} PRIVATE MKW_X86_CPU_PROFILE_V2=1)
        endif()
    endif()
endfunction()

function(mkw_configure_object_target target)
    target_include_directories(${target} PRIVATE
        "${MKW_RUNTIME_SOURCE_DIR}/include"
        "${MKW_RUNTIME_SOURCE_DIR}/src"
        # Workspace root, so translator output is spelled "generated/<x>.h"
        # instead of a ../ chain whose depth depends on the includer.
        "${MKW_RUNTIME_SOURCE_DIR}/.."
        "${MKW_AURORA_DIR}/include")
    target_compile_definitions(${target} PRIVATE
        TARGET_PC)
    set_target_properties(${target} PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
endfunction()

# scope is PRIVATE for a compiled target and INTERFACE for the imported archive.
function(mkw_runtime_link_dependencies target scope)
    target_link_libraries(${target} ${scope}
        aurora::gx aurora::pad aurora::si aurora::vi aurora::mtx aurora::thp)
    target_link_libraries(${target} ${scope} mkw_platform mkw::pugixml mkw::toml11 mkw::cryptopp mkw::mbedtls)
    if(MKW_PLATFORM_WINDOWS)
        target_link_libraries(${target} ${scope} shell32 windowsapp bthprops)
    elseif(MKW_PLATFORM_LINUX OR MKW_PLATFORM_MACOS_X86_64)
        # ${CMAKE_DL_LIBS} for music_attenuation.cpp's dlopen of libdbus-1 (MPRIS
        # media monitoring). Empty on platforms where dl* is already in libc/libSystem.
        target_link_libraries(${target} ${scope} mkw::libco ${CMAKE_DL_LIBS})
    endif()
    if(MKW_PLATFORM_MACOS)
        # CoreAudio framework is required for automatic music muting on macOS.
        target_link_libraries(${target} ${scope} "${MKW_COREAUDIO_FRAMEWORK}")
        if(MKW_PLATFORM_MACOS_X86_64)
            # libco is used by Intel macOS. Apple Silicon uses the local
            # x18-safe assembly backend and therefore does not define mkw::libco.
            target_link_libraries(${target} ${scope} mkw::libco)
        endif()
    endif()
endfunction()

# Compiles the runtime sources as an OBJECT or STATIC library.
function(mkw_add_runtime_library target type)
    set(_sources ${ARGN})
    add_library(${target} ${type} ${_sources})
    mkw_configure_object_target(${target})
    target_compile_features(${target} PRIVATE cxx_std_20)
    target_compile_definitions(${target} PRIVATE
        SDL_MAIN_HANDLED
        _DISABLE_STRING_ANNOTATION _DISABLE_VECTOR_ANNOTATION)
    mkw_runtime_link_dependencies(${target} PRIVATE)

    if(MKW_CPPWINRT_INCLUDE_DIR)
        if(NOT EXISTS "${MKW_CPPWINRT_INCLUDE_DIR}/winrt/base.h")
            message(FATAL_ERROR
                "MKW_CPPWINRT_INCLUDE_DIR does not contain winrt/base.h: ${MKW_CPPWINRT_INCLUDE_DIR}")
        endif()
        target_include_directories(${target} PRIVATE "${MKW_CPPWINRT_INCLUDE_DIR}")
    endif()

    # Keep runtime unity units small and semantically related. The old generated-TU
    # batch size put all 57 native runtime sources into one memory-heavy compiler job.
    foreach(source IN LISTS _sources)
        get_filename_component(source_name "${source}" NAME_WE)
        string(REPLACE "\\" "/" source_normalized "${source}")
        if(source_normalized MATCHES "/hle/gx/")
            set(runtime_group "gx_bridge")
        elseif(source_name MATCHES "network|socket|dns|dwc|ios")
            set(runtime_group "network_ios")
        elseif(source_name MATCHES "os_|system|memory|fiber|scheduler")
            set(runtime_group "guest_system")
        elseif(source_name MATCHES "debug|trace|prof")
            set(runtime_group "diagnostics")
        else()
            string(SHA256 source_hash "${source_name}")
            string(SUBSTRING "${source_hash}" 0 4 source_hash_prefix)
            math(EXPR runtime_bucket "0x${source_hash_prefix} % 8")
            set(runtime_group "runtime_${runtime_bucket}")
        endif()
        set_source_files_properties("${source}" PROPERTIES UNITY_GROUP "${runtime_group}")
    endforeach()
    # PPC semantic sources are excluded from unity/PCH and configured in CMakeLists.txt.
    set_target_properties(${target} PROPERTIES UNITY_BUILD ON UNITY_BUILD_MODE GROUP)
    target_precompile_headers(${target} PRIVATE "${MKW_RUNTIME_SOURCE_DIR}/include/mkw_pch.h")
    mkw_apply_common_compile_options(${target})
    mkw_apply_baseline_arch(${target})
endfunction()

function(mkw_import_prebuilt_runtime target archive)
    add_library(${target} STATIC IMPORTED GLOBAL)
    set_target_properties(${target} PROPERTIES IMPORTED_LOCATION "${archive}")
    mkw_runtime_link_dependencies(${target} INTERFACE)
endfunction()

# Sets MKW_RUNTIME_USE_PREBUILT in the caller. The archive replaces the
# runtime's objects only when it was built from exactly this workspace's
# runtime inputs for the same x86-64 profile.
function(mkw_select_runtime_provider)
    set(_archive "${MKW_NP_RUNTIME_ARCHIVE_PATH}")
    set(_exists FALSE)
    if(_archive AND EXISTS "${_archive}")
        set(_exists TRUE)
    endif()
    set(_fingerprint "")
    if(_exists AND MKW_USE_PREBUILT_RUNTIME AND MKW_HOST_IS_X86_64 AND
       MKW_NP_RUNTIME_CPU_PROFILE STREQUAL MKW_X86_CPU_PROFILE)
        mkw_runtime_fingerprint(_fingerprint "${MKW_RUNTIME_SOURCE_DIR}"
            "${MKW_CPPWINRT_INCLUDE_DIR}" "${MKW_X86_CPU_PROFILE}")
    endif()
    mkw_runtime_prebuilt_decision(_use _reason
        ARCHIVE "${_archive}"
        ENABLED "${MKW_USE_PREBUILT_RUNTIME}"
        X86_64 "${MKW_HOST_IS_X86_64}"
        ARCHIVE_EXISTS "${_exists}"
        PACKAGE_PROFILE "${MKW_NP_RUNTIME_CPU_PROFILE}"
        PROFILE "${MKW_X86_CPU_PROFILE}"
        PACKAGE_FINGERPRINT "${MKW_NP_RUNTIME_FINGERPRINT}"
        FINGERPRINT "${_fingerprint}")
    if(NOT MKW_NATIVE_PREBUILT_DIR)
        set(_reason "no native prebuilt package is configured")
    endif()

    if(_use)
        message(STATUS "Native runtime: precompiled archive ${_archive} (${_reason})")
        # Any runtime edit must re-run configure so the mismatch is noticed.
        mkw_runtime_fingerprint_inputs(_inputs "${MKW_RUNTIME_SOURCE_DIR}")
        list(TRANSFORM _inputs PREPEND "${MKW_RUNTIME_SOURCE_DIR}/")
        if(MKW_CPPWINRT_INCLUDE_DIR)
            list(APPEND _inputs "${MKW_CPPWINRT_INCLUDE_DIR}/winrt/base.h")
        endif()
        set_property(DIRECTORY "${CMAKE_SOURCE_DIR}" APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_inputs})
    else()
        message(STATUS "Native runtime: compiled from source (${_reason})")
    endif()
    set(MKW_RUNTIME_USE_PREBUILT ${_use} PARENT_SCOPE)
endfunction()
