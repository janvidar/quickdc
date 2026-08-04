#
## CMake build for QuickDC
## Copyright (c) 2001-2010, Jan Vidar Krey
#

cmake_minimum_required(VERSION 3.16)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

project(quickdc
    VERSION 0.4.99
    DESCRIPTION "Portable p2p application for ADC/NMDC networks"
    LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------

# RelWithDebInfo is the default for single-config generators. Debug used to be,
# because it was what turned the QDBG/QERR tracing on - QUICKDC_TRACE does that
# now, so the build type is free to mean what it says everywhere else. It is
# also what packagers and CI key off, and Debug leaves NDEBUG undefined, which
# makes the assert in Hash::Manager::removeJob() a live process abort.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING "Build type" FORCE)
endif()
set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
    Debug Release RelWithDebInfo MinSizeRel)

option(QUICKDC_BUILD_TESTS "Build the test programs and the autotest suite" ON)

# core/debug/dbg.h compiles the QDBG/QNET/QSEARCH/QHUB macros only when DEBUG is
# defined, and what they write is not fit to leave on a user's disk: hub
# passwords, peer addresses and every protocol line in both directions. Asking
# for it is therefore explicit, and separate from the build type.
option(QUICKDC_TRACE
    "Compile in the QDBG/QNET/QHUB tracing subsystem (writes secrets to disk)" OFF)

# SecurityManager::isAuthorized() used to return true for any peer on the
# loopback before looking at a clearance, a token or an ID, under `#if 1` with a
# FIXME asking whether it should be limited to debug builds - so it was in
# release builds too. Any local account, and anything reached through an SSH or
# SOCKS forward to the peer port, could take whatever this client would upload.
#
# Kept as an option because it is genuinely useful when driving two instances on
# one machine, but off, and noisy when it is on.
option(QUICKDC_ALLOW_LOOPBACK_AUTH
    "Authorise any peer connecting over the loopback, with no clearance (INSECURE)" OFF)

if(QUICKDC_TRACE)
    message(WARNING
        "QUICKDC_TRACE is ON: this build writes hub passwords (config/preferences.cpp "
        "and network/adc/server/acl.cpp), peer addresses and raw protocol payloads to "
        "files under ~/.quickdc/debug. Do not distribute it.")
endif()

option(QUICKDC_SANITIZE
    "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

# Defines quickdc_harden(), and probes what this toolchain supports. Included
# here rather than in core/ so that the probe results are reported once and the
# test targets can use the same function.
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(QuickDCHardening)

# ---------------------------------------------------------------------------
# TLS
#
# There is no such thing as a QuickDC without TLS. ADCS, the TLS fingerprinting
# the protocol probe does on an inbound connection, and KEYP certificate
# pinning are all unconditional, so there is no switch guarding them.
#
# Samurai requires the same library and owns the version floors: OpenSSL 3.0 or
# newer, or LibreSSL 3.5 or newer, which FindOpenSSL reports under the same
# name and imported targets. Asking here as well means a missing library is a
# configure error against QuickDC rather than a link failure at the end of a
# build.
# ---------------------------------------------------------------------------

find_package(OpenSSL REQUIRED)

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

# Samurai owns the sanitizer flags and applies them PUBLIC to both the compile
# and the link line, so forwarding the request is all QuickDC has to do - its
# own targets inherit them by linking samurai. Has to be set before samurai is
# configured, since it is that project's cache variable. Unconditional, so that
# turning QUICKDC_SANITIZE back off also turns samurai's off.
set(SAMURAI_SANITIZE ${QUICKDC_SANITIZE} CACHE BOOL "Build samurai with sanitizers" FORCE)

# Samurai exports DEBUG PUBLIC, since dbg.h is one of its public headers, so its
# tracing has to be asked for on the same terms as QuickDC's - otherwise the
# QDBG macros in core/ resolve against a library that did not compile the sinks
# they call. Forwarded unconditionally, as SAMURAI_SANITIZE is, so that turning
# QUICKDC_TRACE back off turns samurai's off too.
set(SAMURAI_TRACE ${QUICKDC_TRACE} CACHE BOOL "Build samurai with tracing" FORCE)

add_subdirectory(samurai)

# ---------------------------------------------------------------------------
# Build identification
#
# PRODUCT, VERSION, SYSTEM, CPU and BUILD, which the sources compile against.
# ---------------------------------------------------------------------------

set(QUICKDC_PRODUCT "QuickDC")
set(QUICKDC_SYSTEM  "${CMAKE_SYSTEM_NAME}")
set(QUICKDC_CPU     "${CMAKE_SYSTEM_PROCESSOR}")

# BUILD is always defined: network/adc/cid.cpp and
# network/adc/server/adchubservice.cpp use it unconditionally. --always so that
# a tagless clone still works.
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

    #
    # The test generator and runtime, from the copy pinned inside samurai rather
    # than whatever happens to be on PATH.
    #
    # find_program(exotic) used to resolve this, which made the build depend on
    # the machine: an older exotic earlier in PATH carries a different runtime, so
    # a .tcc using an assertion added later compiled or did not depending on the
    # environment. The submodule is the version this tree is tested against.
    #
    # samurai adds the same directory, but only when its own tests are enabled -
    # and this build turns those off - so it is added here instead. Both of
    # exotic's options default to off for a nested build, so this contributes no
    # install rules and does not build exotic's own regression suite.
    #
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/samurai/third_party/exotic/CMakeLists.txt)
        add_subdirectory(samurai/third_party/exotic
            ${CMAKE_CURRENT_BINARY_DIR}/exotic)
    endif()
endif()

if(QUICKDC_ALLOW_LOOPBACK_AUTH)
    message(WARNING
        "QUICKDC_ALLOW_LOOPBACK_AUTH is ON: every peer connection from 127.0.0.1 "
        "is authorised with no clearance, no token and no matching ID. Any local "
        "process - or anything forwarded to the peer port over SSH - can download "
        "the whole share. Do not ship a build with this on.")
endif()

add_subdirectory(core)

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------
#
# Until now `cmake --install` installed samurai and nothing of QuickDC, so
# anything built against this library - a front end, in particular - had to reach
# into the source tree for both the archive and the headers. That makes every
# consumer depend on the build directory's layout, and gives them no way to say
# which version they were built against.
#
# Every header under core/ is installed, not a curated subset. The API headers
# reach a dozen others through their own includes - api/hub.h needs
# network/hubsession.h to declare a member, config/preferences.h needs
# iniparser.h - and the boundary between those and the rest has never been
# drawn in this tree. Installing everything is honest about that: it says the
# whole of core/ is what a consumer may reach for, rather than implying a
# curated surface that was really just whatever the closure happened to pull in.

# No INCLUDES DESTINATION: the target already carries the same path through its
# INSTALL_INTERFACE, and naming it here as well exports it twice.
install(TARGETS quickdc
    EXPORT quickdcTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})

# Layout preserved, so an installed header's own "network/user.h" resolves the
# same way it does in the source tree.
install(DIRECTORY core/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/quickdc
    FILES_MATCHING
        PATTERN "*.h"
        # Neither is part of anything a consumer compiles against.
        PATTERN "test" EXCLUDE
        PATTERN "documentation" EXCLUDE)

install(EXPORT quickdcTargets
    FILE quickdcTargets.cmake
    NAMESPACE quickdc::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/quickdc)

configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/quickdcConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/quickdcConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/quickdc)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/quickdcConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    # Nothing about this library's interface is stable yet, so a consumer that
    # asked for one version gets an exact match or nothing.
    COMPATIBILITY ExactVersion)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/quickdcConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/quickdcConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/quickdc)

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------

message(STATUS "")
message(STATUS "QuickDC ${PROJECT_VERSION} configured:")
message(STATUS "  Build type      : ${CMAKE_BUILD_TYPE}")
message(STATUS "  Build id        : ${QUICKDC_BUILD_ID}")
message(STATUS "  Target          : ${QUICKDC_SYSTEM}/${QUICKDC_CPU}")
message(STATUS "  Loopback auth   : ${QUICKDC_ALLOW_LOOPBACK_AUTH}")
message(STATUS "  Samurai         : static (in-tree submodule)")
message(STATUS "  TLS             : mandatory (backend reported above)")
if(QUICKDC_SANITIZE)
    # Samurai reports SAMURAI_SANITIZE_LEAKS just above, but that only reaches
    # its own ctest environment and a QuickDC build does not configure samurai's
    # tests, so leak checking here is whatever AddressSanitizer defaults to.
    message(STATUS "  Sanitizers      : address, undefined")
    message(STATUS "                    (ASAN_OPTIONS=detect_leaks=0 disables leak checks)")
else()
    message(STATUS "  Sanitizers      : OFF")
endif()
message(STATUS "  Build tests     : ${QUICKDC_BUILD_TESTS}")
message(STATUS "  Tracing         : ${QUICKDC_TRACE}")
list(LENGTH QUICKDC_HARDEN_COMPILE _qdc_harden_count)
message(STATUS "  Hardening       : ${_qdc_harden_count} compile flag(s), "
                                   "${QUICKDC_HARDEN_DEFINES}")
message(STATUS "  Pedantic        : ${QUICKDC_PEDANTIC}")
message(STATUS "")
