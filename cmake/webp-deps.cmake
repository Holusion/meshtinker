# libwebp's decoder, for image::decode (docs/images.md#formats). Downloaded at
# configure time from its pinned release archive, checked against the SHA-256
# sum below, built as a static library in the build directory and compiled
# into the commands that decode images, as cmake/avif-deps.cmake does for
# libavif (vendor/README.md#downloaded-at-build-time).

include(ExternalProject)

set(MESHTINKER_DOWNLOAD_DIR "${CMAKE_BINARY_DIR}/third_party/downloads"
    CACHE PATH "Where the archives of downloaded dependencies are kept")

set(meshtinker_webp_version 1.6.0)
set(meshtinker_webp_url "https://storage.googleapis.com/downloads.webmproject.org/releases/webp/libwebp-${meshtinker_webp_version}.tar.gz")
set(meshtinker_webp_sha256 e4ab7009bf0629fd11982d4c2aa83964cf244cffba7347ecd39019a9e38c4564)

# The decoder library only (libwebpdecoder), always optimized, without the
# encoder, the programs or the extras.
set(meshtinker_webp_prefix "${CMAKE_BINARY_DIR}/third_party/webp")
set(meshtinker_webp_library "${meshtinker_webp_prefix}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}webpdecoder${CMAKE_STATIC_LIBRARY_SUFFIX}")
ExternalProject_Add(meshtinker-libwebp
    URL "${meshtinker_webp_url}"
    URL_HASH SHA256=${meshtinker_webp_sha256}
    DOWNLOAD_DIR "${MESHTINKER_DOWNLOAD_DIR}"
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    PREFIX "${meshtinker_webp_prefix}"
    CMAKE_ARGS
        "-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}"
        -DCMAKE_BUILD_TYPE=Release
        "-DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>"
        -DCMAKE_INSTALL_LIBDIR=lib
        -DBUILD_SHARED_LIBS=OFF
        -DWEBP_BUILD_ANIM_UTILS=OFF -DWEBP_BUILD_CWEBP=OFF -DWEBP_BUILD_DWEBP=OFF -DWEBP_BUILD_GIF2WEBP=OFF
        -DWEBP_BUILD_IMG2WEBP=OFF -DWEBP_BUILD_VWEBP=OFF -DWEBP_BUILD_WEBPINFO=OFF -DWEBP_BUILD_WEBPMUX=OFF
        -DWEBP_BUILD_EXTRAS=OFF -DWEBP_BUILD_LIBWEBPMUX=OFF -DWEBP_USE_THREAD=OFF
        -DCMAKE_DISABLE_FIND_PACKAGE_PNG=ON -DCMAKE_DISABLE_FIND_PACKAGE_JPEG=ON
        -DCMAKE_DISABLE_FIND_PACKAGE_TIFF=ON -DCMAKE_DISABLE_FIND_PACKAGE_GIF=ON
    BUILD_BYPRODUCTS "${meshtinker_webp_library}"
    LOG_CONFIGURE ON LOG_BUILD ON LOG_INSTALL ON LOG_OUTPUT_ON_FAILURE ON)

file(MAKE_DIRECTORY "${meshtinker_webp_prefix}/include")
add_library(meshtinker-webp-codec STATIC IMPORTED GLOBAL)
set_target_properties(meshtinker-webp-codec PROPERTIES
    IMPORTED_LOCATION "${meshtinker_webp_library}"
    INTERFACE_INCLUDE_DIRECTORIES "${meshtinker_webp_prefix}/include")
target_link_libraries(meshtinker-webp-codec INTERFACE $<$<NOT:$<PLATFORM_ID:Windows>>:m>)
add_dependencies(meshtinker-webp-codec meshtinker-libwebp)
