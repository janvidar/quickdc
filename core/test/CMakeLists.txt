#
## QuickDC test and example programs
## Copyright (c) 2001-2008, Jan Vidar Krey
#

# These are interactive tools rather than automated tests, so they are built but
# not registered with CTest - the real automated suite is in autotests/.

add_executable(testfile file.cpp)
add_executable(testshare share.cpp)
add_executable(testlock lock.cpp)

if(NOT WIN32)
    add_executable(testdebug debug.cpp)
endif()

set(QUICKDC_TEST_APPS
    testfile testshare testlock)
if(NOT WIN32)
    list(APPEND QUICKDC_TEST_APPS testdebug)
endif()

# A name left here after its source was retired makes the configure fail with a
# confusing message about a target that does not exist, so it is checked first.
foreach(app IN LISTS QUICKDC_TEST_APPS)
 if(NOT TARGET ${app})
 message(FATAL_ERROR
 "QuickDC: QUICKDC_TEST_APPS names '${app}', which no add_executable() "
 "declares. It was probably left behind when its test was converted to "
 "core/test/exotic.")
 endif()
endforeach()

foreach(app IN LISTS QUICKDC_TEST_APPS)
 # quickdc pulls in samurai (and thus OpenSSL, zlib, bzip2 and pthread)
 # transitively, so this single link is enough.
 target_link_libraries(${app} PRIVATE quickdc::quickdc)

 set_target_properties(${app} PROPERTIES
 CXX_STANDARD 20
 CXX_STANDARD_REQUIRED ON
 CXX_EXTENSIONS OFF)

 quickdc_harden(${app})
endforeach()

# Unlike their neighbours these are self-contained, so they do get to run.

add_subdirectory(autotests)
add_subdirectory(exotic)
