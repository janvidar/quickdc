#
## QuickDC test and example programs
## Copyright (c) 2001-2008, Jan Vidar Krey
#

# These are interactive tools rather than automated tests, so they are built but
# not registered with CTest - the real automated suite is in autotests/.

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
add_executable(testxml   xml.cpp)
add_executable(testadccommand adccommand.cpp)
add_executable(testfilelist filelist.cpp)
add_executable(testacl   acl.cpp)
add_executable(testqueue queue.cpp)
add_executable(testqueuecomplete queuecomplete.cpp)
add_executable(testsources sources.cpp)
add_executable(testusers users.cpp)
add_executable(testhubsession hubsession.cpp)
add_executable(testdchubsession dchubsession.cpp)
add_executable(testsecurity security.cpp)
add_executable(testclientsession clientsession.cpp)
add_executable(testdcclientsession dcclientsession.cpp)
add_executable(testfilelistgen filelistgen.cpp)
add_executable(testtransfer transfer.cpp)
add_executable(testchunkmap chunkmap.cpp)
add_executable(testadcparser adcparser.cpp)
add_executable(testhashcache hashcache.cpp)
add_executable(testsearch search.cpp)
add_executable(testdctext dctext.cpp)
add_executable(testconfig config.cpp)
add_executable(testprobe probe.cpp)
add_executable(testcid cid.cpp)
add_executable(testadchub adchub.cpp)

# testdebug needs console.cpp for its output helpers.
add_executable(testdebug debug.cpp cli/console.cpp)

set(QUICKDC_TEST_APPS
    quickdc_cli testfile testshare testlock hublist testdebug testzupload
    testleafhashes testxml testadccommand testfilelist testacl testqueue
    testqueuecomplete testsources testusers testhubsession testdchubsession
    testsecurity testclientsession testdcclientsession testfilelistgen testtransfer
    testchunkmap testadcparser testhashcache
    testsearch testdctext testconfig testprobe testcid testadchub)

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
add_test(NAME hublist COMMAND hublist)
add_test(NAME zupload COMMAND testzupload)
add_test(NAME leafhashes COMMAND testleafhashes)
add_test(NAME xml COMMAND testxml)
add_test(NAME adccommand COMMAND testadccommand)
add_test(NAME filelist COMMAND testfilelist)
add_test(NAME acl COMMAND testacl)
add_test(NAME queue COMMAND testqueue)
add_test(NAME queuecomplete COMMAND testqueuecomplete)
add_test(NAME sources COMMAND testsources)
add_test(NAME users COMMAND testusers)
add_test(NAME hubsession COMMAND testhubsession)
add_test(NAME dchubsession COMMAND testdchubsession)
add_test(NAME security COMMAND testsecurity)
add_test(NAME clientsession COMMAND testclientsession)
add_test(NAME dcclientsession COMMAND testdcclientsession)
add_test(NAME filelistgen COMMAND testfilelistgen)
add_test(NAME transfer COMMAND testtransfer)
add_test(NAME chunkmap COMMAND testchunkmap)
add_test(NAME adcparser COMMAND testadcparser)
add_test(NAME hashcache COMMAND testhashcache)
add_test(NAME search COMMAND testsearch)
add_test(NAME dctext COMMAND testdctext)
add_test(NAME config COMMAND testconfig)
add_test(NAME probe COMMAND testprobe)
add_test(NAME cid COMMAND testcid)
add_test(NAME adchub COMMAND testadchub)

add_subdirectory(autotests)
