# Keep libgit2's build options and compiler flags out of Tortilla's scope.
function(tortilla_fetch_libgit2)
    include(FetchContent)
    set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
    set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)
    set(CMAKE_POLICY_DEFAULT_CMP0126 NEW)
    set(BUILD_SHARED_LIBS OFF)
    set(BUILD_TESTS OFF)
    set(BUILD_CLI OFF)
    set(BUILD_EXAMPLES OFF)
    set(BUILD_FUZZERS OFF)
    set(USE_SSH OFF)
    set(USE_HTTP_PARSER builtin)
    set(USE_BUNDLED_ZLIB ON)
    if(WIN32)
        set(USE_HTTPS WinHTTP)
        set(REGEX_BACKEND builtin)
    endif()
    if(MSVC)
        # Match Tortilla's CMAKE_MSVC_RUNTIME_LIBRARY, including Debug.
        set(STATIC_CRT ON)
    endif()

    # Same release and archive hash as vcpkg baseline 40a678a's libgit2 port.
    FetchContent_Declare(tortilla_libgit2
        URL https://github.com/libgit2/libgit2/archive/refs/tags/v1.9.7.tar.gz
        URL_HASH SHA512=96924a4fd87669ad91d40669946cd249b646f3ce85380fc12b553b6c20338ff3c1df5faa6f168b2ca12ed20c8309116a05021f9710ee7ff61e5b8a249172b0ba
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(tortilla_libgit2)
    # Upstream's static package target only exports its installed includes.
    target_include_directories(libgit2package SYSTEM INTERFACE
        "$<BUILD_INTERFACE:${tortilla_libgit2_SOURCE_DIR}/include>")
    add_library(libgit2::libgit2package ALIAS libgit2package)
endfunction()
