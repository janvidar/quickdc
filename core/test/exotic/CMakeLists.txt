#
## The QuickDC test suite (exotic/*.tcc)
##
## One exotic_add_tests() target per file, rather than one target for all of
## them. exotic's per-file EXO_SETUP hooks are documented as being able to have
## two fixtures alive at once when --sort interleaves files, and while each core
## runs on an event loop of its own, the fixtures still share what belongs to
## the process: the TLS keys, the local hub's record of which endpoints were
## checked, and the scratch files a case points a core at. A target per file
## makes interleaving impossible by construction.
##
## DISCOVER registers one CTest entry per EXO_TEST, so a failure names the case,
## `ctest -R` selects it, and a crash takes down that test rather than the run.
#

## Said the way the autotest suite beside this one says it, and for the same
## reason: without exotic the calls below are not a missing library but an
## unknown command, which stops the configure with a message about CMake syntax
## rather than about a checkout that is not there.
if(NOT COMMAND exotic_add_tests)
    if(QUICKDC_REQUIRE_AUTOTESTS)
        message(FATAL_ERROR
            "QuickDC: 'exotic' is not available and QUICKDC_REQUIRE_AUTOTESTS is "
            "ON. Clone https://github.com/janvidar/exotic beside this checkout, "
            "or set EXOTIC_SOURCE_DIR.")
    endif()
    message(WARNING
        "QuickDC: 'exotic' is not available - SKIPPING the exotic suite, and "
        "ctest will still report success. Clone "
        "https://github.com/janvidar/exotic beside this checkout, or set "
        "EXOTIC_SOURCE_DIR, to enable it.")
    return()
endif()

set(QUICKDC_EXOTIC_TESTS
    attachments.tcc
    adcparser.tcc
    dctext.tcc
    filelistgen.tcc
    probe.tcc
    hashcache.tcc
    search.tcc
    security.tcc
    token.tcc
    chunkmap.tcc
    chatarchive.tcc
    boardindex.tcc
    boardkeeper.tcc
    config.tcc
    hublist.tcc
    hubdirectory.tcc
    transfer.tcc
    users.tcc
    adccommand.tcc
    filelist.tcc
    queue.tcc
    acl.tcc
    sources.tcc
    xml.tcc
    cid.tcc
    adchub.tcc
    clientsession.tcc
    dchubsession.tcc
    dcclientsession.tcc
    hubsession.tcc
    bbs_client.tcc
    bbs_hub.tcc
    leafhashes.tcc
    lock.tcc
    portforward.tcc
    reachability.tcc
    dropcheck.tcc
    networkmode.tcc
    searchtokens.tcc
    subscription.tcc
    inbox.tcc
    service.tcc
    networkplan.tcc
    migrations.tcc
    mappingpolicy.tcc
    localaddresses.tcc
    queuecomplete.tcc
    twocores.tcc
    sharefolders.tcc
    userlists.tcc
    chatmedia.tcc
    zupload.tcc
    settingskeeper.tcc)

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

    # Drop is a PRIVATE dependency of the library, so its include directories
    # stop at the library and never reach a consumer - which is the point, since
    # nothing outside reads a verdict in Drop's types. Two tests do have business
    # with it and need them back: dropcheck constructs the class declared in
    # network/dropcheck.h, the one QuickDC header that names Drop, and probe
    # answers the DROP challenge on the peer port, so what a challenge line
    # needs is part of what it is testing. Named here rather than added to the
    # whole list, so that a test with no such business still cannot include it.
    if(stem STREQUAL "dropcheck" OR stem STREQUAL "probe")
        target_link_libraries(quickdc_test_${stem} PRIVATE drop::drop)
    endif()

    quickdc_harden(quickdc_test_${stem})
endforeach()
