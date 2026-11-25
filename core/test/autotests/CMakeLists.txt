#
## The QuickDC autotest suite (*.tcc)
##
## Built with exotic (https://github.com/janvidar/exotic), which is pinned as a
## submodule inside samurai and added to this build by the top level. A clone
## made without --recurse-submodules leaves it absent, which is now the only way
## it can be missing - it used to be resolved with find_program(), so the build
## depended on which exotic happened to be first on PATH.
##
## The skip is a WARNING rather than a STATUS because it silently removes every
## assertion below and ctest still reports the remaining tests green, so a build
## that skipped them looks exactly like one that ran them. Anything that gates on
## the suite should set QUICKDC_REQUIRE_AUTOTESTS so the absence is fatal instead
## of quiet; the CI workflow does.
#

option(QUICKDC_REQUIRE_AUTOTESTS
    "Fail the configure if 'exotic' is missing, rather than skipping the suite" OFF)

if(NOT COMMAND exotic_add_tests)
    if(QUICKDC_REQUIRE_AUTOTESTS)
        message(FATAL_ERROR
            "QuickDC: 'exotic' is not available and QUICKDC_REQUIRE_AUTOTESTS is "
            "ON. Clone https://github.com/janvidar/exotic beside this checkout, "
            "or set EXOTIC_SOURCE_DIR.")
    endif()
    message(WARNING
        "QuickDC: 'exotic' is not available - SKIPPING the autotest suite, and "
        "ctest will still report success. Clone "
        "https://github.com/janvidar/exotic beside this checkout, or set "
        "EXOTIC_SOURCE_DIR, to enable it.")
    return()
endif()

#
# MODE AUTO compiles each .tcc as its own translation unit and lets each test
# register itself before main(). Nothing is generated, which removes two things
# this file used to work around: the generator's --standalone output needed an
# autotest.h to exist even though it inlines one, so an empty stub had to be
# written into the build directory; and the inlined runtime carries a vsnprintf()
# forwarder that -Werror=format-nonliteral rejected, so that one flag had to be
# relaxed for this target. Neither applies when the runtime is a library.
#
# DISCOVER registers one CTest entry per EXO_TEST instead of one for the whole
# binary, so a failure names the case and `ctest -R` can select it.
#
exotic_add_tests(quickdc_autotest
    LANGUAGE CXX
    MODE AUTO
    DISCOVER
    SOURCES
        adc_bbs.tcc
        adc_natt.tcc
        adc_parser.tcc
        filelist.tcc
        filesource.tcc
        keyprint.tcc
        nmdc.tcc
        preferences.tcc
        queue.tcc
        queuefile.tcc
        securitymanager.tcc
        socketmonitor.tcc
        stdc.tcc
        user.tcc
    LIBRARIES quickdc::quickdc)

set_target_properties(quickdc_autotest PROPERTIES
    OUTPUT_NAME autotest
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF)

quickdc_harden(quickdc_autotest)
