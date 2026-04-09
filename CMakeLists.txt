cmake_minimum_required(VERSION 3.20.0)

# Zephyr package
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(tinygs_nRF52)

# Enable C++ support for RadioLib
set(CMAKE_CXX_STANDARD 11)

# Add RadioLib as a library
file(GLOB_RECURSE RADIOLIB_SRCS "lib/RadioLib/src/*.cpp" "lib/RadioLib/src/*.c")
add_library(radiolib STATIC ${RADIOLIB_SRCS})
target_include_directories(radiolib PUBLIC lib/RadioLib/src)
zephyr_library_link_libraries(radiolib)

# Application Sources
target_sources(app PRIVATE 
    src/main.cpp
    src/hal/ZephyrHal.cpp
)

# App Include Directories
target_include_directories(app PRIVATE src/hal lib/RadioLib/src)

