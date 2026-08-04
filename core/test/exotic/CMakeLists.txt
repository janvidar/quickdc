#
## The QuickDC test suite (exotic/*.tcc)
##
## One exotic_add_tests() target per file, rather than one target for all of
## them, because QuickDC::Core is a process-wide singleton: its constructor
## assigns a global that the next one would overwrite, and its destructor sets
## that global back to null. exotic's per-file EXO_SETUP hooks are documented as
## being able to have two fixtures alive at once when --sort interleaves files,
## which for a Core would mean one file's teardown clearing the pointer the next
## file's tests are still using. A target per file makes interleaving impossible
## by construction.
##
## DISCOVER registers one CTest entry per EXO_TEST, so a failure names the case,
## `ctest -R` selects it, and a crash takes down that test rather than the run.
#

set(QUICKDC_EXOTIC_TESTS
    adcparser.tcc
    dctext.tcc
    filelistgen.tcc
    probe.tcc
    hashcache.tcc
    search.tcc
    security.tcc
    transfer.tcc
    users.tcc)

foreach(input IN LISTS QUICKDC_EXOTIC_TESTS)
    cmake_path(GET input STEM stem)

    exotic_add_tests(quickdc_test_${stem}
        LANGUAGE CXX
        MODE AUTO
        DISCOVER
        SOURCES ${input}
        LIBRARIES quickdc::quickdc)

    set_target_properties(quickdc_test_${stem} PROPERTIES
        OUTPUT_NAME test_${stem}
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF)

    quickdc_harden(quickdc_test_${stem})
endforeach()
