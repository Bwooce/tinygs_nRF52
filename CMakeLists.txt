cmake_minimum_required(VERSION 3.20.0)

# External Zephyr modules
list(APPEND ZEPHYR_EXTRA_MODULES ${CMAKE_CURRENT_SOURCE_DIR}/../esp-iot-log)

# Zephyr package
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(tinygs_nRF52)

# Enable C++ support for RadioLib
set(CMAKE_CXX_STANDARD 20)

# RadioLib: only compile SX126x + core (saves ~85KB vs all radio modules).
# In v3.3 sysbuild, zephyr_library_named() is rejected in app-mode CMakeLists,
# so we add the sources directly to the app target instead.
file(GLOB RADIOLIB_CORE "lib/RadioLib/src/*.cpp")
file(GLOB RADIOLIB_SX126x "lib/RadioLib/src/modules/SX126x/*.cpp")
file(GLOB RADIOLIB_UTILS "lib/RadioLib/src/utils/*.cpp")
file(GLOB RADIOLIB_PHYSICAL "lib/RadioLib/src/protocols/PhysicalLayer/*.cpp")
set(RADIOLIB_SRCS
    ${RADIOLIB_CORE}
    ${RADIOLIB_SX126x}
    ${RADIOLIB_UTILS}
    ${RADIOLIB_PHYSICAL}
    lib/RadioLib/src/hal/Zephyr/ZephyrHal.cpp
)

# Application Sources
target_sources(app PRIVATE
    ${RADIOLIB_SRCS}
    src/main.cpp
    src/hal/test_ZephyrHal.cpp
    src/tinygs_protocol.cpp
    src/tinygs_json.cpp
    src/tinygs_config.cpp
    src/tinygs_display.cpp
    src/font8x16.c
    src/bitcode.c
    lib/AioP13/src/AioP13.cpp
)

# App Include Directories
target_include_directories(app PRIVATE src/hal lib/RadioLib/src lib/ArduinoJson/src lib/AioP13/src)

# v3.3 PSA path: try without the legacy mbedTLS RSA force-compile workaround.
# v2.6 PSA returned FEATURE_UNAVAILABLE for PKCS1v15 RSA verify, forcing us
# to compile pk.c/pk_wrap.c/pkparse.c/rsa.c with -UMBEDTLS_USE_PSA_CRYPTO.
# v3.3's PSA crypto is reportedly fixed; if mqtt.tinygs.com TLS handshake
# fails on hardware, restore the RSA_FIX_SRCS block from git history.

# Generate UF2 for Adafruit/Heltec bootloaders
set_target_properties(app PROPERTIES BUILD_OUTPUT_UF2 TRUE)


