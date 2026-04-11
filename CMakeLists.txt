cmake_minimum_required(VERSION 3.20.0)

# Zephyr package
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(tinygs_nRF52)

# Enable C++ support for RadioLib
set(CMAKE_CXX_STANDARD 20)

# Add RadioLib as a Zephyr library
zephyr_library_named(radiolib)

file(GLOB_RECURSE RADIOLIB_SRCS "lib/RadioLib/src/*.cpp" "lib/RadioLib/src/*.c")
# EXCLUDE HAL sources since we use our own ZephyrHal
list(FILTER RADIOLIB_SRCS EXCLUDE REGEX "lib/RadioLib/src/hal/.*")

zephyr_library_sources(${RADIOLIB_SRCS})
zephyr_library_include_directories(lib/RadioLib/src)

# Application Sources
target_sources(app PRIVATE
    src/main.cpp
    src/hal/ZephyrHal.cpp
    src/hal/test_ZephyrHal.cpp
    src/tinygs_protocol.cpp
)

# App Include Directories
target_include_directories(app PRIVATE src/hal lib/RadioLib/src lib/ArduinoJson/src)

# mbedTLS user config (for RSA/PKCS1 defines not exposed via NCS PSA Kconfig)
# This must be a global include so nrf_security's mbedtls compilation can find it
zephyr_include_directories(${CMAKE_CURRENT_SOURCE_DIR}/src)

# Force-compile RSA source files — NCS nrf_security PSA path doesn't compile
# them even though MBEDTLS_RSA_C is defined in mbedtls-user-config.h.
# mqtt.tinygs.com requires ECDHE-RSA cipher suite.
# Force-compile PK + RSA sources with MBEDTLS_RSA_C and WITHOUT
# MBEDTLS_USE_PSA_CRYPTO so the legacy RSA verify path is used.
# The PSA path (CC310/Oberon) returns FEATURE_UNAVAILABLE for
# PKCS1v15 RSA signature verification.
set(RSA_FIX_SRCS
    ${ZEPHYR_MBEDTLS_MODULE_DIR}/library/pk.c
    ${ZEPHYR_MBEDTLS_MODULE_DIR}/library/pk_wrap.c
    ${ZEPHYR_MBEDTLS_MODULE_DIR}/library/pkparse.c
    ${ZEPHYR_MBEDTLS_MODULE_DIR}/library/rsa.c
    ${ZEPHYR_MBEDTLS_MODULE_DIR}/library/rsa_alt_helpers.c
)
target_sources(app PRIVATE ${RSA_FIX_SRCS})
# Force legacy RSA path (not PSA) for these files
set_source_files_properties(${RSA_FIX_SRCS} PROPERTIES
    COMPILE_OPTIONS "-UMBEDTLS_USE_PSA_CRYPTO"
)
zephyr_link_libraries(-Wl,--allow-multiple-definition)

# Link RadioLib to the app
# zephyr_library_named automatically handles some linking, 
# but we might still need to link it to app.
target_link_libraries(app PUBLIC radiolib)

# Generate UF2 for Adafruit/Heltec bootloaders
set_target_properties(app PROPERTIES BUILD_OUTPUT_UF2 TRUE)


