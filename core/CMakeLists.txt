#
## The QuickDC core library (libquickdc.a)
## Copyright (c) 2001-2010, Jan Vidar Krey
#

# NOTE: one .cpp file under core/ is deliberately absent, being
# unreferenced/unfinished: api/cli.cpp
#
# debug/dbg.cpp defines QuickDC_Debug/QERR/QNET, which nothing else provides;
# samurai's own logging is under samurai_debug() and friends, so the two do not
# collide.
set(QUICKDC_SOURCES
    bbs/boardindex.cpp
    bbs/poststore.cpp
    chat/archive.cpp
    network/chunkmap.cpp
    network/commandparser.cpp
    network/commandproxy.cpp
    network/connection.cpp
    network/connectionmanager.cpp
    network/downloadqueue.cpp
    network/filesource.cpp
    network/hublist.cpp
    network/hubsession.cpp
    network/hubmanager.cpp
    network/keyprint.cpp
    network/leafhashes.cpp
    network/dropcheck.cpp
    network/portforward.cpp
    network/reachability.cpp
    network/searchtokens.cpp
    network/probe.cpp
    network/securitymanager.cpp
    network/transfer.cpp
    network/transfermanager.cpp
    network/user.cpp
    network/usermanager.cpp
    network/dc/clientcommands.cpp
    network/dc/clientsession.cpp
    network/dc/commands.cpp
    network/dc/dccommandparser.cpp
    network/dc/dccommandproxy.cpp
    network/dc/dcconnection.cpp
    network/dc/dcfile.cpp
    network/dc/hubcommands.cpp
    network/dc/hubsession.cpp
    network/dc/lock.cpp
    network/dc/tag.cpp
    network/dc/textescape.cpp
    network/adc/adcbbs.cpp
    network/adc/adccommandparser.cpp
    network/adc/adccommandproxy.cpp
    network/adc/adccommands.cpp
    network/adc/adcudp.cpp
    network/adc/cid.cpp
    network/adc/client/adcclientsession.cpp
    network/adc/client/adchubsession.cpp
    network/adc/server/acl.cpp
    network/adc/server/boards.cpp
    network/adc/server/claimcheck.cpp
    network/adc/server/commands.cpp
    network/adc/server/hub.cpp
    network/adc/server/service.cpp
    network/adc/server/user.cpp
    network/adc/parser.cpp
    share/dcfilelist.cpp
    share/sharedir.cpp
    share/sharefile.cpp
    share/sharemanager.cpp
    share/searchmanager.cpp
    share/searchrequest.cpp
    share/searchresult.cpp
    config/backend.cpp
    config/iniparser.cpp
    config/preferences.cpp
    debug/dbg.cpp
    api/core.cpp
    api/hub.cpp
    api/inbox.cpp
    api/networkmode.cpp
    api/server.cpp
    hash/hashmanager.cpp
    hash/hashjob.cpp
    hash/hashcache.cpp
    hash/hashstorage.cpp
    xml/node.cpp
    xml/writer.cpp)

add_library(quickdc STATIC ${QUICKDC_SOURCES})
add_library(quickdc::quickdc ALIAS quickdc)

# Produce libquickdc.a.
set_target_properties(quickdc PROPERTIES OUTPUT_NAME quickdc)

# Sources include their siblings by path from core/, e.g. "debug/dbg.h" and
# "quickdc.h". PUBLIC so the test programs get it too.
#
# Split across the two interfaces because the build-tree path is meaningless to
# anything consuming an installed package: without the generator expressions the
# exported target would carry an absolute path into whatever machine built it.
# The installed headers keep the same layout under include/quickdc, so a consumer
# writes the same #include lines this tree does.
target_include_directories(quickdc PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/quickdc>)

# C++20 to match samurai, which exports cxx_std_20 as a usage requirement and
# whose headers use std::span, std::optional and class template argument
# deduction.
set_target_properties(quickdc PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON)

# PUBLIC because these appear in code reachable from the headers the test
# programs compile against.
#
# _FILE_OFFSET_BITS=64 and DEBUG are deliberately not defined here: samurai
# already exports them as PUBLIC definitions, and defining them again would risk
# disagreeing with the library about struct layout. DEBUG in particular follows
# SAMURAI_TRACE, which the top level sets from QUICKDC_TRACE, so asking for
# tracing once turns it on for both trees.
target_compile_definitions(quickdc
    PUBLIC
        PRODUCT="${QUICKDC_PRODUCT}"
        VERSION="${PROJECT_VERSION}"
        SYSTEM="${QUICKDC_SYSTEM}"
        CPU="${QUICKDC_CPU}"
        BUILD="${QUICKDC_BUILD_ID}")

# ZLIB/BZIP2 gate the file list compression in share/sharemanager.cpp, which
# calls samurai's Samurai::IO::BZip2Compressor. Samurai compiles its codecs
# unconditionally, so these are always safe to enable.
target_compile_definitions(quickdc PRIVATE ZLIB BZIP2)

# PUBLIC so the security test can assert whichever behaviour it was built with,
# rather than only the default one.
if(QUICKDC_ALLOW_LOOPBACK_AUTH)
    target_compile_definitions(quickdc PUBLIC QUICKDC_ALLOW_LOOPBACK_AUTH)
endif()

# samurai::samurai_static carries its own include directories, the OpenSSL,
# zlib, bzip2 and pthread link dependencies, the C++20 usage requirement, and
# the _FILE_OFFSET_BITS / DEBUG definitions. OpenSSL comes through as PUBLIC
# because samurai's tlsfactory-openssl.h is a public header that includes
# <openssl/ssl.h>. PUBLIC here too: QuickDC's headers include samurai headers,
# so consumers need all of it as well.
target_link_libraries(quickdc PUBLIC samurai::samurai_static)

# The reachability measurement. PRIVATE, and network/dropcheck.h is the only
# header that names Drop at all: the verdict crosses out of here in QuickDC's
# own types, through api/dropverdict.h, so that neither an exported target nor
# a front end has to have Drop on its include path to read one.
target_link_libraries(quickdc PRIVATE drop::drop)

# -Wall -Wextra, the curated -Werror set, and the stack, bounds and branch
# protections. See cmake/QuickDCHardening.cmake.
quickdc_harden(quickdc)

if(QUICKDC_BUILD_TESTS)
    add_subdirectory(test)
endif()
