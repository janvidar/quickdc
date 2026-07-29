#
## The QuickDC core library (libquickdc.a)
## Copyright (c) 2001-2010, Jan Vidar Krey
#

# NOTE: three .cpp files under core/ are deliberately absent, being
# unreferenced/unfinished:
#   api/cli.cpp, network/adc/server/adctun.cpp, xml/parser.cpp
#
# debug/dbg.cpp defines QuickDC_Debug/QERR/QNET, which nothing else provides;
# samurai's own logging is under samurai_debug() and friends, so the two do not
# collide.
set(QUICKDC_SOURCES
    network/chunkmap.cpp
    network/commandparser.cpp
    network/commandproxy.cpp
    network/connection.cpp
    network/connectionmanager.cpp
    network/downloadqueue.cpp
    network/filesource.cpp
    network/hubmanager.cpp
    network/keyprint.cpp
    network/leafhashes.cpp
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
    network/adc/server/adchub.cpp
    network/adc/server/adchubservice.cpp
    network/adc/server/adchubuser.cpp
    network/adc/parser.cpp
    network/http/client.cpp
    network/http/entity.cpp
    network/http/header.cpp
    network/http/httpconnection.cpp
    network/http/httpparser.cpp
    network/http/pipeline.cpp
    network/http/request.cpp
    network/http/response.cpp
    network/http/server.cpp
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
    api/server.cpp
    hash/hashmanager.cpp
    hash/hashjob.cpp
    hash/hashcache.cpp
    hash/hashstorage.cpp
    xml/node.cpp
    xml/reader.cpp
    xml/writer.cpp)

add_library(quickdc STATIC ${QUICKDC_SOURCES})
add_library(quickdc::quickdc ALIAS quickdc)

# Produce libquickdc.a.
set_target_properties(quickdc PROPERTIES OUTPUT_NAME quickdc)

# Sources include their siblings by path from core/, e.g. "debug/dbg.h" and
# "quickdc.h". PUBLIC so the test programs get it too.
target_include_directories(quickdc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

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
# disagreeing with the library about struct layout.
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

# samurai::samurai_static carries its own include directories, the OpenSSL,
# zlib, bzip2 and pthread link dependencies, the C++20 usage requirement, and
# the _FILE_OFFSET_BITS / DEBUG definitions. OpenSSL comes through as PUBLIC
# because samurai's tlsfactory-openssl.h is a public header that includes
# <openssl/ssl.h>. PUBLIC here too: QuickDC's headers include samurai headers,
# so consumers need all of it as well.
target_link_libraries(quickdc PUBLIC samurai::samurai_static)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(quickdc PRIVATE -Wall -Wextra)
endif()

if(QUICKDC_BUILD_TESTS)
    add_subdirectory(test)
endif()
