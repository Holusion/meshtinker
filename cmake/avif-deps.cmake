# libavif with libaom's encoder, for `avif` (docs/avif.md#encoder), and the
# nasm assembler that libaom's x86 code needs. They are downloaded at
# configure time from pinned release archives, checked against the SHA-256
# sums below, built as static libraries in the build directory, and compiled
# into meshtinker-avif: nothing is installed on the system, and nothing is
# loaded at run time (vendor/README.md#downloaded-at-build-time).
#
# Downloads go to MESHTINKER_DOWNLOAD_DIR, which several build directories, or
# an offline machine, can share: an archive already there with the right sum
# isn't downloaded again.

include(ExternalProject)

set(MESHTINKER_DOWNLOAD_DIR "${CMAKE_BINARY_DIR}/third_party/downloads"
    CACHE PATH "Where the archives of downloaded dependencies are kept")

set(meshtinker_nasm_version 3.02)
set(meshtinker_nasm_url "https://www.nasm.us/pub/nasm/releasebuilds/${meshtinker_nasm_version}/nasm-${meshtinker_nasm_version}.tar.xz")
set(meshtinker_nasm_sha256 87336eba53b4acfe917424ab5d500d2b0054d9f5148d35c2273ccf2cfb712f0d)

set(meshtinker_aom_version 3.15.3)
set(meshtinker_aom_url "https://storage.googleapis.com/aom-releases/libaom-${meshtinker_aom_version}.tar.gz")
set(meshtinker_aom_sha256 12c01dc731ed7b5a999e28120b45bf58a002ba3435a4ba2cfbc1e084a27c9c04)

set(meshtinker_avif_version 1.4.2)
set(meshtinker_avif_url "https://github.com/AOMediaCodec/libavif/archive/refs/tags/v${meshtinker_avif_version}.tar.gz")
set(meshtinker_avif_sha256 2b645287340ba5a631d268b551dc2d72bd73ac33335962dd36dcdb6d8366921d)

set(meshtinker_third_party "${CMAKE_BINARY_DIR}/third_party")
find_program(MESHTINKER_MAKE NAMES gmake make REQUIRED)

# nasm: its own configure script, with its bundled zlib, so that the build
# doesn't depend on the system's. Only the `nasm` program is built.
ExternalProject_Add(meshtinker-nasm
    URL "${meshtinker_nasm_url}"
    URL_HASH SHA256=${meshtinker_nasm_sha256}
    DOWNLOAD_DIR "${MESHTINKER_DOWNLOAD_DIR}"
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    PREFIX "${meshtinker_third_party}/nasm"
    BUILD_IN_SOURCE ON
    CONFIGURE_COMMAND <SOURCE_DIR>/configure --without-zlib "CC=${CMAKE_C_COMPILER}"
    BUILD_COMMAND "${MESHTINKER_MAKE}" nasm
    INSTALL_COMMAND "${CMAKE_COMMAND}" -E copy <SOURCE_DIR>/nasm <INSTALL_DIR>/bin/nasm
    BUILD_BYPRODUCTS "${meshtinker_third_party}/nasm/bin/nasm"
    LOG_CONFIGURE ON LOG_BUILD ON LOG_OUTPUT_ON_FAILURE ON)
set(meshtinker_nasm "${meshtinker_third_party}/nasm/bin/nasm")

# libaom: encoder and decoder (the decoder reads AVIF back in tests), always
# optimized, without its programs, tests or documentation.
set(meshtinker_aom_prefix "${meshtinker_third_party}/aom")
set(meshtinker_aom_library "${meshtinker_aom_prefix}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}aom${CMAKE_STATIC_LIBRARY_SUFFIX}")
ExternalProject_Add(meshtinker-aom
    URL "${meshtinker_aom_url}"
    URL_HASH SHA256=${meshtinker_aom_sha256}
    DOWNLOAD_DIR "${MESHTINKER_DOWNLOAD_DIR}"
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    PREFIX "${meshtinker_aom_prefix}"
    DEPENDS meshtinker-nasm
    CMAKE_ARGS
        "-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}"
        "-DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
        "-DCMAKE_ASM_NASM_COMPILER=${meshtinker_nasm}"
        -DCMAKE_BUILD_TYPE=Release
        "-DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>"
        -DCMAKE_INSTALL_LIBDIR=lib
        -DBUILD_SHARED_LIBS=OFF
        -DENABLE_DOCS=0 -DENABLE_EXAMPLES=0 -DENABLE_TESTDATA=0 -DENABLE_TESTS=0 -DENABLE_TOOLS=0
    BUILD_BYPRODUCTS "${meshtinker_aom_library}"
    LOG_CONFIGURE ON LOG_BUILD ON LOG_INSTALL ON LOG_OUTPUT_ON_FAILURE ON)

# libavif on that libaom, without libyuv (its own RGB to YUV conversion), its
# programs or tests. pkg-config is turned off so that a system libaom can't be
# picked instead.
set(meshtinker_avif_prefix "${meshtinker_third_party}/avif")
set(meshtinker_avif_library "${meshtinker_avif_prefix}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}avif${CMAKE_STATIC_LIBRARY_SUFFIX}")
ExternalProject_Add(meshtinker-libavif
    URL "${meshtinker_avif_url}"
    URL_HASH SHA256=${meshtinker_avif_sha256}
    DOWNLOAD_DIR "${MESHTINKER_DOWNLOAD_DIR}"
    DOWNLOAD_NAME "libavif-${meshtinker_avif_version}.tar.gz"
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    PREFIX "${meshtinker_avif_prefix}"
    DEPENDS meshtinker-aom
    CMAKE_ARGS
        "-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}"
        "-DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
        -DCMAKE_BUILD_TYPE=Release
        "-DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>"
        -DCMAKE_INSTALL_LIBDIR=lib
        -DBUILD_SHARED_LIBS=OFF
        -DAVIF_CODEC_AOM=SYSTEM
        "-DAOM_INCLUDE_DIR=${meshtinker_aom_prefix}/include"
        "-DAOM_LIBRARY=${meshtinker_aom_library}"
        -DCMAKE_DISABLE_FIND_PACKAGE_PkgConfig=ON
        -DAVIF_LIBYUV=OFF -DAVIF_LIBSHARPYUV=OFF -DAVIF_ZLIBPNG=OFF -DAVIF_JPEG=OFF -DAVIF_LIBXML2=OFF
        -DAVIF_BUILD_APPS=OFF -DAVIF_BUILD_TESTS=OFF
    BUILD_BYPRODUCTS "${meshtinker_avif_library}"
    LOG_CONFIGURE ON LOG_BUILD ON LOG_INSTALL ON LOG_OUTPUT_ON_FAILURE ON)

# Imported targets for the static libraries. Their include directories must
# exist at configure time, before the builds fill them. Imported include
# directories are system ones, so the libraries' headers raise no warnings.
file(MAKE_DIRECTORY "${meshtinker_aom_prefix}/include" "${meshtinker_avif_prefix}/include")
find_package(Threads REQUIRED)
add_library(meshtinker-aom-lib STATIC IMPORTED GLOBAL)
set_target_properties(meshtinker-aom-lib PROPERTIES
    IMPORTED_LOCATION "${meshtinker_aom_library}"
    INTERFACE_INCLUDE_DIRECTORIES "${meshtinker_aom_prefix}/include")
target_link_libraries(meshtinker-aom-lib INTERFACE Threads::Threads $<$<NOT:$<PLATFORM_ID:Windows>>:m>)
add_dependencies(meshtinker-aom-lib meshtinker-aom)
add_library(meshtinker-avif-codec STATIC IMPORTED GLOBAL)
set_target_properties(meshtinker-avif-codec PROPERTIES
    IMPORTED_LOCATION "${meshtinker_avif_library}"
    INTERFACE_INCLUDE_DIRECTORIES "${meshtinker_avif_prefix}/include")
target_link_libraries(meshtinker-avif-codec INTERFACE meshtinker-aom-lib)
add_dependencies(meshtinker-avif-codec meshtinker-libavif)
