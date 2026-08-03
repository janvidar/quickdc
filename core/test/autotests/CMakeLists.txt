#
## The QuickDC autotest suite (*.tcc)
##
## These are real automated tests, but they have to be run through the
## "exotic" generator (https://github.com/janvidar/exotic) to become
## compilable C++. If the generator is not installed, skip them rather than
## fail the build.
#

find_program(QUICKDC_EXOTIC_EXECUTABLE exotic
    DOC "Path to the exotic test generator")

if(NOT QUICKDC_EXOTIC_EXECUTABLE)
    message(STATUS
        "QuickDC: 'exotic' generator not found - skipping the autotest suite.")
    return()
endif()

set(QUICKDC_EXOTIC_INPUTS
    adc_parser.tcc
    keyprint.tcc
    preferences.tcc
    queue.tcc
    stdc.tcc
    user.tcc)

set(QUICKDC_EXOTIC_INPUT_PATHS "")
foreach(input IN LISTS QUICKDC_EXOTIC_INPUTS)
    list(APPEND QUICKDC_EXOTIC_INPUT_PATHS ${CMAKE_CURRENT_SOURCE_DIR}/${input})
endforeach()

set(QUICKDC_EXOTIC_GENERATED ${CMAKE_CURRENT_BINARY_DIR}/generated_tests.cpp)

add_custom_command(
    OUTPUT ${QUICKDC_EXOTIC_GENERATED}
    COMMAND ${CMAKE_COMMAND}
        -DEXOTIC=${QUICKDC_EXOTIC_EXECUTABLE}
        -DSRCDIR=${CMAKE_CURRENT_SOURCE_DIR}
        -DOUTPUT=${QUICKDC_EXOTIC_GENERATED}
        "-DINPUTS=${QUICKDC_EXOTIC_INPUTS}"
        -P ${PROJECT_SOURCE_DIR}/cmake/RunExotic.cmake
    DEPENDS ${QUICKDC_EXOTIC_INPUT_PATHS} ${PROJECT_SOURCE_DIR}/cmake/RunExotic.cmake
    COMMENT "Generating QuickDC autotest suite"
    VERBATIM)

# exotic --standalone inlines the contents of autotest.h (behind its own
# include guard) but still emits an #include for it, so the header only has to
# exist to satisfy the preprocessor. Prefer the real one; fall back to an empty
# stand-in so the suite builds without exotic's source tree present.
find_path(QUICKDC_EXOTIC_INCLUDE_DIR autotest.h
    PATH_SUFFIXES exotic exotic/src
    DOC "Directory containing exotic's autotest.h")

if(QUICKDC_EXOTIC_INCLUDE_DIR)
    set(QUICKDC_EXOTIC_HEADER_DIR ${QUICKDC_EXOTIC_INCLUDE_DIR})
else()
    set(QUICKDC_EXOTIC_HEADER_DIR ${CMAKE_CURRENT_BINARY_DIR}/exotic-stub)
    file(MAKE_DIRECTORY ${QUICKDC_EXOTIC_HEADER_DIR})
    file(WRITE ${QUICKDC_EXOTIC_HEADER_DIR}/autotest.h
        "/* Intentionally empty: exotic --standalone already inlines the real\n"
        "   autotest.h, but still emits an #include for it. See CMakeLists.txt. */\n")
endif()

add_executable(quickdc_autotest ${QUICKDC_EXOTIC_GENERATED})

target_include_directories(quickdc_autotest PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}   # the .tcc files are #included by name
    ${QUICKDC_EXOTIC_HEADER_DIR})

target_link_libraries(quickdc_autotest PRIVATE quickdc::quickdc)

set_target_properties(quickdc_autotest PROPERTIES
    OUTPUT_NAME autotest
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF)

quickdc_harden(quickdc_autotest)

add_test(NAME autotest COMMAND quickdc_autotest)
