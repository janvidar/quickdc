#
## qdc: the headless front end
## Copyright (c) 2026, Jan Vidar Krey
#

add_executable(qdc
    main.cpp
    commands.cpp
    eventjson.cpp
    freeport.cpp
    json.cpp
    runner.cpp
    script.cpp
    session.cpp)

target_link_libraries(qdc PRIVATE quickdc::quickdc)

set_target_properties(qdc PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF)

quickdc_harden(qdc)

if(QUICKDC_BUILD_TESTS)
    # Not a program but a rule: only runner.cpp may see the core.
    add_test(NAME cli_isolation
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/only-the-protocol.sh
                ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/core)

    set(scenario ${CMAKE_CURRENT_SOURCE_DIR}/tests/scenario.sh)
    set(scripts ${CMAKE_CURRENT_SOURCE_DIR}/tests)

    add_test(NAME cli_two_cores_chat
        COMMAND ${scenario} 0 - $<TARGET_FILE:qdc>
                --core a --core b --free-port PORT --free-port PORT_B --quiet
                ${scripts}/two-cores-chat.qdc)

    foreach(name search search-attributed filelist download-active download-passive download-passive-stale-active
                 download-tls share-live completed chat-media)
        string(REPLACE "-" "_" test ${name})
        add_test(NAME cli_${test}
            COMMAND ${scenario} 0 - $<TARGET_FILE:qdc>
                    --core a --core b --free-port PORT --free-port PORT_B --quiet
                    ${scripts}/${name}.qdc)
        set_tests_properties(cli_${test} PROPERTIES TIMEOUT 120)
    endforeach()

    add_test(NAME cli_favourites
        COMMAND ${scenario} 0 - $<TARGET_FILE:qdc> --quiet ${scripts}/favourites.qdc)

    add_test(NAME cli_recent_hubs
        COMMAND ${scenario} 0 - $<TARGET_FILE:qdc> --free-port PORT --quiet
                ${scripts}/recent-hubs.qdc)
    set_tests_properties(cli_favourites cli_recent_hubs PROPERTIES TIMEOUT 120)

    add_test(NAME cli_wait_times_out
        COMMAND ${scenario} 1 "timed out after 300ms waiting for UserJoined nick=nobody"
                $<TARGET_FILE:qdc> --quiet ${scripts}/wait-times-out.qdc)

    add_test(NAME cli_syntax_error
        COMMAND ${scenario} 2 "syntax-error.qdc:6: unknown command 'conect'"
                $<TARGET_FILE:qdc> ${scripts}/syntax-error.qdc)

    add_test(NAME cli_bad_wait
        COMMAND ${scenario} 2 "bad-wait.qdc:3: there is no event 'UserJoind'"
                $<TARGET_FILE:qdc> ${scripts}/bad-wait.qdc)

    add_test(NAME cli_late_preset
        COMMAND ${scenario} 2 "late-preset.qdc:4: 'preset' is taken before the cores start"
                $<TARGET_FILE:qdc> ${scripts}/late-preset.qdc)

    # Each core makes a 3072-bit certificate on its first run, which is slow to
    # generate on a loaded machine.
    set_tests_properties(cli_two_cores_chat cli_wait_times_out PROPERTIES TIMEOUT 120)
endif()
