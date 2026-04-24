cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(tinygs_p13_propagator_tests)

target_sources(app PRIVATE
    src/main.cpp
    ../../lib/AioP13/src/AioP13.cpp
)

target_include_directories(app PRIVATE
    ../../lib/AioP13/src
)
