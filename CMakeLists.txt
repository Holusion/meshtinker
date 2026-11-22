cmake_minimum_required(VERSION 3.20)

project(meshtinker VERSION 0.1.0 LANGUAGES C CXX)

# An empty build type compiles without optimization, which makes large inputs
# impractically slow. Keep debug info so stack sampling remains useful.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

include(CTest)

# Off by default: a compiler version the project hasn't been checked with can
# warn about code that is fine, and a build from source shouldn't fail on it.
# CI and agents turn it on, with the reference toolchain (.github/workflows/ci.yml).
option(MESHTINKER_WARNINGS_AS_ERRORS "Treat compiler warnings in project code as errors" OFF)
option(MESHTINKER_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
# `avif` builds libavif, libaom and nasm from release archives downloaded by
# the build (cmake/avif-deps.cmake). Off for builds without network access
# and without a download folder to reuse.
option(MESHTINKER_AVIF "Build the avif command (downloads and builds nasm, libaom and libavif)" ON)

# Executables link libstdc++ and libgcc statically so that only the C library
# is loaded at run time. Sanitizer runtimes are shared libraries, so sanitized
# builds keep the default.
if(NOT APPLE AND NOT MSVC AND NOT MESHTINKER_SANITIZE)
    set(meshtinker_static_runtime_default ON)
else()
    set(meshtinker_static_runtime_default OFF)
endif()
option(MESHTINKER_STATIC_RUNTIME "Link libstdc++ and libgcc statically" ${meshtinker_static_runtime_default})

# compile_commands.json feeds the `tidy` target and editors.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Settings shared by every target, including vendored code.
function(meshtinker_configure_base target)
    target_compile_features(${target} PRIVATE cxx_std_17)
    set_target_properties(${target} PROPERTIES CXX_EXTENSIONS OFF)
    # Bounds-check standard containers in debug builds.
    target_compile_definitions(${target} PRIVATE $<$<CONFIG:Debug>:_GLIBCXX_ASSERTIONS>)
    if(MESHTINKER_SANITIZE)
        target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(${target} PRIVATE -fsanitize=address,undefined)
    endif()
endfunction()

# Project code: strict warnings, as errors with MESHTINKER_WARNINGS_AS_ERRORS.
function(meshtinker_configure target)
    meshtinker_configure_base(${target})
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
        if(MESHTINKER_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic
            # Implicit narrowing and sign changes, such as size_t to uint32_t.
            -Wconversion -Wsign-conversion
            -Wshadow -Wold-style-cast -Wcast-qual -Wundef -Wformat=2
            -Wnon-virtual-dtor -Woverloaded-virtual -Wextra-semi -Wimplicit-fallthrough
            # Free functions need a prior declaration: a header or an unnamed namespace.
            -Wmissing-declarations
            $<$<CXX_COMPILER_ID:GNU>:-Wduplicated-cond -Wduplicated-branches -Wlogical-op>
            $<$<CXX_COMPILER_ID:Clang,AppleClang>:-Wmissing-prototypes -Wcomma -Wloop-analysis>
        )
        if(MESHTINKER_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()

function(meshtinker_configure_executable target)
    meshtinker_configure(${target})
    if(MESHTINKER_STATIC_RUNTIME)
        target_link_options(${target} PRIVATE -static-libstdc++ -static-libgcc)
    endif()
endfunction()

# Vendored code is not ours to fix; keep its warnings out of the build output.
add_library(meshtinker-vendor STATIC vendor/impl/cgltf.cpp vendor/impl/stb.cpp)
meshtinker_configure_base(meshtinker-vendor)
target_compile_options(meshtinker-vendor PRIVATE $<IF:$<CXX_COMPILER_ID:MSVC>,/W0,-w>)
target_include_directories(meshtinker-vendor SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/vendor")

file(GLOB meshoptimizer_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/vendor/meshoptimizer/src/*.cpp")
add_library(meshoptimizer STATIC ${meshoptimizer_sources})
meshtinker_configure_base(meshoptimizer)
target_compile_options(meshoptimizer PRIVATE $<IF:$<CXX_COMPILER_ID:MSVC>,/W0,-w>)
target_include_directories(meshoptimizer SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/vendor/meshoptimizer/src")
target_link_libraries(meshtinker-vendor PUBLIC meshoptimizer)

# Draco's decoder only: the sources of Draco's default decoder build, with a
# hand-written draco_features.h (vendor/README.md).
file(GLOB_RECURSE draco_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/vendor/draco/src/*.cc")
add_library(draco-decoder STATIC ${draco_sources})
meshtinker_configure_base(draco-decoder)
target_compile_options(draco-decoder PRIVATE $<IF:$<CXX_COMPILER_ID:MSVC>,/W0,-w>)
target_include_directories(draco-decoder SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/vendor/draco/src")
target_link_libraries(meshtinker-vendor PUBLIC draco-decoder)

# basis_universal's encoder and transcoder, with Zstandard, as upstream's
# basisu_encoder target builds them (vendor/README.md). Only `ktx` and its
# tests link it, so other commands don't carry it.
find_package(Threads REQUIRED)
set(basisu_dir "${CMAKE_CURRENT_SOURCE_DIR}/vendor/basisu")
# Upstream's ENCODER_LIB_SRC_LIST, with Zstandard and without astcenc.
set(basisu_sources
    encoder/basisu_backend.cpp
    encoder/basisu_basis_file.cpp
    encoder/basisu_comp.cpp
    encoder/basisu_enc.cpp
    encoder/basisu_etc.cpp
    encoder/basisu_frontend.cpp
    encoder/basisu_gpu_texture.cpp
    encoder/basisu_pvrtc1_4.cpp
    encoder/basisu_resampler.cpp
    encoder/basisu_resample_filters.cpp
    encoder/basisu_ssim.cpp
    encoder/basisu_uastc_enc.cpp
    encoder/basisu_bc7e_scalar.cpp
    encoder/basisu_dds_export.cpp
    encoder/basisu_bc7enc.cpp
    encoder/basisu_kernels_sse.cpp
    encoder/basisu_bc15_spmd.cpp
    encoder/basisu_bc15_spmd_sse.cpp
    encoder/basisu_opencl.cpp
    encoder/basisu_uastc_hdr_4x4_enc.cpp
    encoder/basisu_astc_hdr_6x6_enc.cpp
    encoder/basisu_astc_hdr_common.cpp
    encoder/basisu_astc_ldr_common.cpp
    encoder/basisu_astc_ldr_encode.cpp
    encoder/basisu_astc_ldr_fencode.cpp
    encoder/basisu_xbc7_encode.cpp
    encoder/basisu_tinyexr.cpp
    encoder/jpgd.cpp
    encoder/pvpngreader.cpp
    encoder/3rdparty/android_astc_decomp.cpp
    transcoder/basisu_transcoder.cpp
    zstd/zstd.c)
list(TRANSFORM basisu_sources PREPEND "${basisu_dir}/")
add_library(basisu STATIC ${basisu_sources})
meshtinker_configure_base(basisu)
target_compile_options(basisu PRIVATE $<IF:$<CXX_COMPILER_ID:MSVC>,/W0,-w -fno-strict-aliasing>)
target_include_directories(basisu SYSTEM PUBLIC "${basisu_dir}")
target_compile_definitions(basisu PUBLIC BASISD_SUPPORT_KTX2_ZSTD=1 BASISU_SUPPORT_OPENCL=0 PRIVATE BASISU_SUPPORT_ASTCENC=0)
# Upstream builds the whole library with SSE 4.1 on x86, and calls one of its
# kernels without checking the CPU, so it needs x86-64-v2.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
    target_compile_definitions(basisu PUBLIC BASISU_SUPPORT_SSE=1)
    target_compile_options(basisu PRIVATE -msse4.1)
else()
    target_compile_definitions(basisu PUBLIC BASISU_SUPPORT_SSE=0)
endif()
target_link_libraries(basisu PUBLIC Threads::Threads)

# xatlas, for `uv` only.
add_library(xatlas STATIC vendor/xatlas/xatlas.cpp)
meshtinker_configure_base(xatlas)
target_compile_options(xatlas PRIVATE $<IF:$<CXX_COMPILER_ID:MSVC>,/W0,-w>)
target_include_directories(xatlas SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/vendor/xatlas")
target_link_libraries(xatlas PUBLIC Threads::Threads)

# Shared namespaces: common, gltf, cli, analysis. Never depends on a subcommand.
file(GLOB meshtinker_core_sources CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/common/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gltf/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/cli/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/analysis/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/chain/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/image/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/texture/*.cpp"
)
add_library(meshtinker-core STATIC ${meshtinker_core_sources})
meshtinker_configure(meshtinker-core)
target_include_directories(meshtinker-core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_definitions(meshtinker-core PRIVATE MESHTINKER_VERSION="${PROJECT_VERSION}")
target_link_libraries(meshtinker-core PUBLIC meshtinker-vendor)

# meshtinker_add_command(<name>) builds src/commands/<name>/ into meshtinker-<name>.
# Every source except main.cpp also goes into the meshtinker-<name>-lib static
# library, so tests can link a command's internals.
set(meshtinker_commands)
function(meshtinker_add_command name)
    set(dir "${CMAKE_CURRENT_SOURCE_DIR}/src/commands/${name}")
    file(GLOB sources CONFIGURE_DEPENDS "${dir}/*.cpp")
    list(REMOVE_ITEM sources "${dir}/main.cpp")
    add_executable(meshtinker-${name} "${dir}/main.cpp")
    meshtinker_configure_executable(meshtinker-${name})
    if(sources)
        add_library(meshtinker-${name}-lib STATIC ${sources})
        meshtinker_configure(meshtinker-${name}-lib)
        target_link_libraries(meshtinker-${name}-lib PUBLIC meshtinker-core)
        target_link_libraries(meshtinker-${name} PRIVATE meshtinker-${name}-lib)
    else()
        target_link_libraries(meshtinker-${name} PRIVATE meshtinker-core)
    endif()
    install(TARGETS meshtinker-${name} RUNTIME DESTINATION bin)
    set(meshtinker_commands ${meshtinker_commands} meshtinker-${name} PARENT_SCOPE)
endfunction()

meshtinker_add_command(inspect)
meshtinker_add_command(import)
meshtinker_add_command(placeholder)
meshtinker_add_command(simplify)
meshtinker_add_command(weld)
meshtinker_add_command(dedup)
meshtinker_add_command(normals)
meshtinker_add_command(compress)
meshtinker_add_command(export)
meshtinker_add_command(ktx)
target_link_libraries(meshtinker-ktx-lib PUBLIC basisu)
meshtinker_add_command(uv)
target_link_libraries(meshtinker-uv-lib PUBLIC xatlas)
if(MESHTINKER_AVIF)
    include(cmake/avif-deps.cmake)
    meshtinker_add_command(avif)
    target_link_libraries(meshtinker-avif-lib PUBLIC meshtinker-avif-codec)
endif()
find_package(Threads REQUIRED)
target_link_libraries(meshtinker-placeholder-lib PRIVATE Threads::Threads)

# The wrapper is named `meshtinker`, not `meshtinker-meshtinker`.
add_executable(meshtinker src/commands/meshtinker/main.cpp)
meshtinker_configure_executable(meshtinker)
target_link_libraries(meshtinker PRIVATE meshtinker-core)
install(TARGETS meshtinker RUNTIME DESTINATION bin)

# `cmake --build build --target tidy` lints project sources with .clang-tidy.
# Findings are errors. Not part of the default build.
find_program(MESHTINKER_RUN_CLANG_TIDY NAMES run-clang-tidy run-clang-tidy-19 run-clang-tidy-20)
if(MESHTINKER_RUN_CLANG_TIDY)
    add_custom_target(tidy
        COMMAND "${MESHTINKER_RUN_CLANG_TIDY}" -quiet -p "${CMAKE_BINARY_DIR}"
            "-header-filter=^${CMAKE_SOURCE_DIR}/(include|src|test)/"
            -warnings-as-errors=*
            # GCC-only warning flags appear in compile_commands.json.
            -extra-arg=-Wno-unknown-warning-option
            "^${CMAKE_SOURCE_DIR}/(src|test)/"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        COMMENT "Running clang-tidy on project sources"
        USES_TERMINAL
        VERBATIM
    )
endif()

if(BUILD_TESTING)
    # Test executables go in tests/ so that the wrapper never lists them.
    add_library(meshtinker-test-support STATIC test/support/check.cpp test/support/fixtures.cpp)
    meshtinker_configure(meshtinker-test-support)
    target_include_directories(meshtinker-test-support PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/test")
    target_compile_definitions(meshtinker-test-support PUBLIC
        MESHTINKER_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/test/data"
        MESHTINKER_BUILD_DIR="${CMAKE_BINARY_DIR}")
    target_link_libraries(meshtinker-test-support PUBLIC meshtinker-core)

    # meshtinker_add_test(<name> <source> [LIBS...]) builds test/<source> as tests/test-<name>.
    function(meshtinker_add_test name source)
        add_executable(test-${name} test/${source})
        meshtinker_configure(test-${name})
        set_target_properties(test-${name} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
        target_link_libraries(test-${name} PRIVATE meshtinker-test-support ${ARGN})
        add_test(NAME ${name} COMMAND test-${name})
    endfunction()

    # meshtinker_add_script_test(<name> <script>) runs test/<script> in CMake script mode.
    function(meshtinker_add_script_test name script)
        add_test(NAME ${name} COMMAND "${CMAKE_COMMAND}"
            "-DBUILD_DIR=${CMAKE_BINARY_DIR}"
            "-DSOURCE_DIR=${CMAKE_SOURCE_DIR}"
            "-DWORK_DIR=${CMAKE_BINARY_DIR}/test-work/${name}"
            "-DCOMMANDS=${meshtinker_commands}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/test/${script}")
    endfunction()

    meshtinker_add_test(common-process common/process_test.cpp)
    meshtinker_add_test(common-references common/references_test.cpp)
    meshtinker_add_test(common-outputs common/outputs_test.cpp)
    meshtinker_add_test(common-workspace common/workspace_test.cpp)
    meshtinker_add_test(chain chain/chain_test.cpp)
    meshtinker_add_test(gltf-uri gltf/uri_test.cpp)
    meshtinker_add_test(gltf-image-header gltf/image_header_test.cpp)
    meshtinker_add_test(image image/image_test.cpp)
    meshtinker_add_test(gltf-document gltf/document_test.cpp)
    meshtinker_add_test(gltf-writer gltf/writer_test.cpp)
    meshtinker_add_test(gltf-selection gltf/selection_test.cpp)
    meshtinker_add_test(gltf-geometry gltf/geometry_test.cpp)
    meshtinker_add_test(gltf-normalized gltf/normalized_test.cpp)
    meshtinker_add_test(gltf-edit gltf/edit_test.cpp)
    meshtinker_add_test(analysis-coverage analysis/coverage_test.cpp)
    meshtinker_add_test(analysis-fixtures analysis/fixtures_test.cpp)
    meshtinker_add_test(placeholder placeholder/placeholder_test.cpp meshtinker-placeholder-lib)
    target_include_directories(test-placeholder PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    meshtinker_add_test(simplify simplify/simplify_test.cpp meshtinker-simplify-lib)
    target_include_directories(test-simplify PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    meshtinker_add_test(dedup dedup/dedup_test.cpp meshtinker-dedup-lib)
    target_include_directories(test-dedup PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    meshtinker_add_test(weld weld/weld_test.cpp meshtinker-weld-lib)
    target_include_directories(test-weld PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    meshtinker_add_test(normals normals/normals_test.cpp meshtinker-normals-lib meshtinker-weld-lib)
    target_include_directories(test-normals PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    meshtinker_add_test(compress compress/compress_test.cpp meshtinker-compress-lib)
    target_include_directories(test-compress PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    meshtinker_add_test(ktx ktx/ktx_test.cpp meshtinker-ktx-lib)
    meshtinker_add_test(uv uv/uv_test.cpp meshtinker-uv-lib)
    target_include_directories(test-uv PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_include_directories(test-ktx PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    if(MESHTINKER_AVIF)
        meshtinker_add_test(avif avif/avif_test.cpp meshtinker-avif-lib)
        target_include_directories(test-avif PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    endif()
    meshtinker_add_script_test(wrapper commands/wrapper_test.cmake)
    meshtinker_add_test(chain-commands commands/chain_test.cpp)
    if(MESHTINKER_AVIF)
        target_compile_definitions(test-chain-commands PRIVATE MESHTINKER_HAS_AVIF)
    endif()
    meshtinker_add_test(cli-readme-help cli/readme_help_test.cpp)
    meshtinker_add_test(cli-command cli/command_test.cpp)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND MESHTINKER_STATIC_RUNTIME)
        meshtinker_add_script_test(dynamic-libraries build/dynamic_libraries_test.cmake)
    endif()
endif()
