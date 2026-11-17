# Build Android dependencies with the same NDK as the application, never host pkg-config libraries.
if(NOT CMAKE_ANDROID_ARCH_ABI STREQUAL "arm64-v8a")
    message(FATAL_ERROR "Pilon's Android build currently targets arm64-v8a.")
endif()
find_package(Perl REQUIRED)
find_program(PILON_MAKE_EXECUTABLE make REQUIRED)
FetchContent_Declare(pilon_openssl
    URL https://github.com/openssl/openssl/releases/download/openssl-3.5.9/openssl-3.5.9.tar.gz
    URL_HASH SHA256=603f5602e2eef00d77fbd429d34dcd5822bb301757a1bc9cdb24c670f1eb859a
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(pilon_openssl)
file(MAKE_DIRECTORY "${pilon_openssl_BINARY_DIR}/include")
# Android's system OpenSSL must not satisfy Qt's or libssh2's library lookup.
file(CONFIGURE OUTPUT "${pilon_openssl_BINARY_DIR}/pilon.conf" CONTENT
    "my %targets = (\"pilon-android-arm64\" => { inherit_from => [\"android-arm64\"], shlib_variant => \"_3\" });\n")
get_filename_component(PILON_ANDROID_TOOLCHAIN_BIN "${CMAKE_C_COMPILER}" DIRECTORY)
set(PILON_OPENSSL_ENV "ANDROID_NDK_ROOT=${ANDROID_NDK_ROOT}"
    "PATH=${PILON_ANDROID_TOOLCHAIN_BIN}:$ENV{PATH}")
set(PILON_OPENSSL_CONFIGURE_ARGS
    "--config=${pilon_openssl_BINARY_DIR}/pilon.conf" pilon-android-arm64
    "--prefix=${pilon_openssl_BINARY_DIR}/install" --libdir=lib
    -D__ANDROID_API__=28 shared no-tests no-apps no-module
    -Wl,-z,max-page-size=16384)
string(SHA256 PILON_OPENSSL_CONFIG_HASH "${CMAKE_C_COMPILER};${ANDROID_NDK_ROOT};${PILON_OPENSSL_CONFIGURE_ARGS};_3")
if(NOT PILON_OPENSSL_CONFIGURED STREQUAL PILON_OPENSSL_CONFIG_HASH
        OR NOT EXISTS "${pilon_openssl_BINARY_DIR}/configdata.pm")
    execute_process(COMMAND "${CMAKE_COMMAND}" -E env ${PILON_OPENSSL_ENV}
        "${PERL_EXECUTABLE}" "${pilon_openssl_SOURCE_DIR}/Configure" ${PILON_OPENSSL_CONFIGURE_ARGS}
        WORKING_DIRECTORY "${pilon_openssl_BINARY_DIR}"
        RESULT_VARIABLE PILON_OPENSSL_CONFIGURE_RESULT OUTPUT_QUIET)
    if(NOT PILON_OPENSSL_CONFIGURE_RESULT EQUAL 0)
        message(FATAL_ERROR "Unable to configure Android OpenSSL.")
    endif()
    execute_process(COMMAND "${CMAKE_COMMAND}" -E env ${PILON_OPENSSL_ENV}
        "${PILON_MAKE_EXECUTABLE}" build_generated
        WORKING_DIRECTORY "${pilon_openssl_BINARY_DIR}"
        RESULT_VARIABLE PILON_OPENSSL_HEADERS_RESULT OUTPUT_QUIET)
    if(NOT PILON_OPENSSL_HEADERS_RESULT EQUAL 0)
        message(FATAL_ERROR "Unable to generate Android OpenSSL headers.")
    endif()
    set(PILON_OPENSSL_CONFIGURED "${PILON_OPENSSL_CONFIG_HASH}" CACHE INTERNAL "" FORCE)
endif()
set(PILON_ANDROID_OPENSSL_LIBRARIES
    "${pilon_openssl_BINARY_DIR}/libcrypto_3.so" "${pilon_openssl_BINARY_DIR}/libssl_3.so")
add_custom_command(OUTPUT ${PILON_ANDROID_OPENSSL_LIBRARIES}
    COMMAND "${CMAKE_COMMAND}" -E env ${PILON_OPENSSL_ENV}
        "${PILON_MAKE_EXECUTABLE}" -j4 build_libs
    WORKING_DIRECTORY "${pilon_openssl_BINARY_DIR}"
    DEPENDS "${pilon_openssl_BINARY_DIR}/Makefile" VERBATIM)
add_custom_target(pilon_android_openssl DEPENDS ${PILON_ANDROID_OPENSSL_LIBRARIES})
set(OPENSSL_INCLUDE_DIR "${pilon_openssl_BINARY_DIR}/include" CACHE PATH "" FORCE)
set(OPENSSL_CRYPTO_LIBRARY "${pilon_openssl_BINARY_DIR}/libcrypto_3.so" CACHE FILEPATH "" FORCE)
set(OPENSSL_SSL_LIBRARY "${pilon_openssl_BINARY_DIR}/libssl_3.so" CACHE FILEPATH "" FORCE)
add_library(OpenSSL::Crypto SHARED IMPORTED GLOBAL)
add_library(OpenSSL::SSL SHARED IMPORTED GLOBAL)
set_target_properties(OpenSSL::Crypto PROPERTIES IMPORTED_LOCATION "${OPENSSL_CRYPTO_LIBRARY}")
set_target_properties(OpenSSL::SSL PROPERTIES IMPORTED_LOCATION "${OPENSSL_SSL_LIBRARY}"
    INTERFACE_LINK_LIBRARIES OpenSSL::Crypto)
find_package(OpenSSL 3 REQUIRED)
set_target_properties(OpenSSL::Crypto OpenSSL::SSL PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${pilon_openssl_BINARY_DIR}/include;${pilon_openssl_SOURCE_DIR}/include")
add_dependencies(OpenSSL::Crypto pilon_android_openssl)
add_dependencies(OpenSSL::SSL pilon_android_openssl)

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_STATIC_LIBS ON CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(CRYPTO_BACKEND OpenSSL CACHE STRING "" FORCE)
set(ENABLE_ZLIB_COMPRESSION ON CACHE BOOL "" FORCE)
FetchContent_Declare(libssh2
    URL https://www.libssh2.org/download/libssh2-1.11.1.tar.gz
    URL_HASH SHA256=d9ec76cbe34db98eec3539fe2c899d26b0c837cb3eb466a56b0f109cabf658f7
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(libssh2)
set_target_properties(libssh2_static PROPERTIES POSITION_INDEPENDENT_CODE ON)
set(PILON_SSH2_TARGET libssh2_static)
