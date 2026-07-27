#
## CMake build for QuickDC
## Copyright (c) 2001-2010, Jan Vidar Krey
#

cmake_minimum_required(VERSION 3.16)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

project(quickdc
    VERSION 0.4.19
    DESCRIPTION "Portable p2p application for ADC/NMDC networks"
    LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------

# Makefile.system had DEBUG := YES hardcoded, and that is what turns on the
# QDBG/QERR tracing. Keep it as the default for single-config generators.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "Build type" FORCE)
endif()
set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
    Debug Release RelWithDebInfo MinSizeRel)

option(QUICKDC_BUILD_TESTS "Build the test programs and the autotest suite" ON)

# NOTE: core/debug/malloc.cpp (dlmalloc) and core/debug/memdbg.cpp are both
# wrapped entirely in #ifdef QUICKDC_MEMDBG, and nothing in the Makefile build
# ever defined it - the two objects it compiled were empty. This option makes
# the switch actually work; it is off because the tracing allocator overrides
# malloc/free process-wide.
option(QUICKDC_MEMDBG "Build the debug allocator (dlmalloc + allocation tracing)" OFF)

# ---------------------------------------------------------------------------
# Samurai submodule
# ---------------------------------------------------------------------------

if(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/samurai/CMakeLists.txt)
    message(FATAL_ERROR
        "The samurai submodule is not checked out. Run:\n"
        "    git submodule update --init --recursive")
endif()

# QuickDC links samurai statically, so ask for the static flavour only. FORCE
# because these are cache variables owned by the samurai subproject.
set(SAMURAI_BUILD_STATIC ON  CACHE BOOL "Build the samurai static library" FORCE)
set(SAMURAI_BUILD_SHARED OFF CACHE BOOL "Build the samurai shared library" FORCE)

# Samurai's own test and example programs are not part of a QuickDC build.
# Configure samurai standalone if you want them.
set(SAMURAI_BUILD_TESTS OFF CACHE BOOL "Build the samurai test programs" FORCE)

add_subdirectory(samurai)

# ---------------------------------------------------------------------------
# Build identification
#
# Replaces the -DVERSION/-DSYSTEM/-DCPU/-DPRODUCT/-DBUILD block in
# Makefile.common. SYSTEM and ARCH came from uname; CMake has already probed
# the equivalents.
# ---------------------------------------------------------------------------

set(QUICKDC_PRODUCT "QuickDC")
set(QUICKDC_SYSTEM  "${CMAKE_SYSTEM_NAME}")
set(QUICKDC_CPU     "${CMAKE_SYSTEM_PROCESSOR}")

# The Makefile defined BUILD only when "git describe" succeeded, but
# network/adc/cid.cpp and network/adc/server/adchubservice.cpp use it
# unconditionally and only compiled because debug/dbg.h has an #ifndef BUILD
# fallback. Always define it, and use --always so a tagless clone still works.
set(QUICKDC_BUILD_ID "internal")

find_package(Git QUIET)
if(GIT_FOUND AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/.git)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} describe --always --dirty
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        OUTPUT_VARIABLE QUICKDC_GIT_DESCRIBE
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE QUICKDC_GIT_RESULT)
    if(QUICKDC_GIT_RESULT EQUAL 0 AND QUICKDC_GIT_DESCRIBE)
        set(QUICKDC_BUILD_ID "${QUICKDC_GIT_DESCRIBE}")
    endif()
endif()

# ---------------------------------------------------------------------------
# Targets
# ---------------------------------------------------------------------------

if(QUICKDC_BUILD_TESTS)
    enable_testing()
endif()

add_subdirectory(core)

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------

message(STATUS "")
message(STATUS "QuickDC ${PROJECT_VERSION} configured:")
message(STATUS "  Build type      : ${CMAKE_BUILD_TYPE}")
message(STATUS "  Build id        : ${QUICKDC_BUILD_ID}")
message(STATUS "  Target          : ${QUICKDC_SYSTEM}/${QUICKDC_CPU}")
message(STATUS "  Samurai         : static (in-tree submodule)")
message(STATUS "  Debug allocator : ${QUICKDC_MEMDBG}")
message(STATUS "  Build tests     : ${QUICKDC_BUILD_TESTS}")
message(STATUS "")
