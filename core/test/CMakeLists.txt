#
## QuickDC test and example programs
## Copyright (c) 2001-2008, Jan Vidar Krey
#

# Translated from build/Makefile.tests. These are interactive tools rather
# than automated tests, so they are built but not registered with CTest - the
# real automated suite is in autotests/.
#
# NOTE: Makefile.tests also listed test/crash.cpp in test_SOURCES, but no rule
# ever built a binary from it and the file does not exist in the tree.

add_executable(quickdc_cli
    cli/main.cpp
    cli/util.cpp
    cli/hubconnection.cpp
    cli/commands.cpp
    cli/console.cpp)

add_executable(testfile  file.cpp)
add_executable(testshare share.cpp)
add_executable(testlock  lock.cpp)
add_executable(hublist   hublist.cpp)
add_executable(testzupload zupload.cpp)
add_executable(testleafhashes leafhashes.cpp)

# testdebug needs console.cpp for its output helpers.
add_executable(testdebug debug.cpp cli/console.cpp)

set(QUICKDC_TEST_APPS
    quickdc_cli testfile testshare testlock hublist testdebug testzupload
    testleafhashes)

foreach(app IN LISTS QUICKDC_TEST_APPS)
    # quickdc pulls in samurai (and thus OpenSSL, zlib, bzip2 and pthread)
    # transitively, so this single link is the whole of the old REQLIBS.
    target_link_libraries(${app} PRIVATE quickdc::quickdc)

    set_target_properties(${app} PROPERTIES
        CXX_STANDARD 11
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF)

    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${app} PRIVATE -Wall -Wextra)
    endif()
endforeach()

# Unlike their neighbours these are self-contained, so they do get to run.
add_test(NAME zupload COMMAND testzupload)
add_test(NAME leafhashes COMMAND testleafhashes)

add_subdirectory(autotests)
