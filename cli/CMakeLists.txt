#
## qdc: the headless front end
## Copyright (c) 2026, Jan Vidar Krey
#

add_executable(qdc
    main.cpp
    commands.cpp
    eventjson.cpp
    fileserver.cpp
    freeport.cpp
    human.cpp
    json.cpp
    remote.cpp
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

    # These two give both cores one port and learn where each bound from CoreStatus.
    foreach(name two-cores-chat download-active)
        string(REPLACE "-" "_" test ${name})
        add_test(NAME cli_${test}
            COMMAND ${scenario} 0 - $<TARGET_FILE:qdc> --core a --core b --free-port PORT --quiet ${scripts}/${name}.qdc)
        set_tests_properties(cli_${test} PROPERTIES TIMEOUT 120)
    endforeach()

    foreach(name search search-attributed filelist download-passive download-passive-stale-active
                 share-live completed chat-media)
        string(REPLACE "-" "_" test ${name})
        add_test(NAME cli_${test}
            COMMAND ${scenario} 0 - $<TARGET_FILE:qdc>
                    --core a --core b --free-port PORT --free-port PORT_B --quiet
                    ${scripts}/${name}.qdc)
        set_tests_properties(cli_${test} PROPERTIES TIMEOUT 120)
    endforeach()

    # An encrypted transfer ending as it should logs nothing as an error, on either side.
    add_test(NAME cli_download_tls
        COMMAND ${scenario} 0 "!ERROR" $<TARGET_FILE:qdc>
                --core a --core b --free-port PORT --free-port PORT_B --quiet
                ${scripts}/download-tls.qdc)
    set_tests_properties(cli_download_tls PROPERTIES TIMEOUT 120)

    add_test(NAME cli_boards
        COMMAND ${scenario} 0 - $<TARGET_FILE:qdc>
                --core a --core b --core c --free-port PORT --free-port PORT_B --free-port PORT_C
                --quiet ${scripts}/boards.qdc)
    set_tests_properties(cli_boards PROPERTIES TIMEOUT 120)

    add_test(NAME cli_operator
        COMMAND ${scenario} 0 - $<TARGET_FILE:qdc>
                --core a --core b --core c --free-port PORT --free-port PORT_B --free-port PORT_C
                --quiet ${scripts}/operator.qdc)
    set_tests_properties(cli_operator PROPERTIES TIMEOUT 120)

    add_test(NAME cli_redirect
        COMMAND ${scenario} 0 - $<TARGET_FILE:qdc>
                --core a --core b --core c --core d --free-port PORT --free-port PORT_B
                --free-port PORT_C --free-port PORT_D --quiet ${scripts}/redirect.qdc)
    set_tests_properties(cli_redirect PROPERTIES TIMEOUT 120)

    add_test(NAME cli_hub_directory
        COMMAND ${scenario} 0 - $<TARGET_FILE:qdc> --free-port HTTP --free-port CLOSED
                -D LISTS=${scripts}/hublists --quiet ${scripts}/hub-directory.qdc)
    set_tests_properties(cli_hub_directory PROPERTIES TIMEOUT 120)

    add_test(NAME cli_favourites
        COMMAND ${scenario} 0 - $<TARGET_FILE:qdc> --quiet ${scripts}/favourites.qdc)

    add_test(NAME cli_recent_hubs
        COMMAND ${scenario} 0 - $<TARGET_FILE:qdc> --free-port PORT --quiet
                ${scripts}/recent-hubs.qdc)
    add_test(NAME cli_network_changed
        COMMAND ${scenario} 0 - $<TARGET_FILE:qdc> --quiet ${scripts}/network-changed.qdc)
    set_tests_properties(cli_network_changed PROPERTIES TIMEOUT 120)

    add_test(NAME cli_local_hub_join
        COMMAND ${scenario} 0 - $<TARGET_FILE:qdc> --free-port PORT --free-port PORT_B --quiet
                ${scripts}/local-hub-join.qdc)
    set_tests_properties(cli_local_hub_join PROPERTIES TIMEOUT 120)

    add_test(NAME cli_silent_hub
        COMMAND ${scenario} 0 - $<TARGET_FILE:qdc> --free-port SILENT --quiet ${scripts}/silent-hub.qdc)
    set_tests_properties(cli_silent_hub PROPERTIES TIMEOUT 120)

    add_test(NAME cli_session_hubs
        COMMAND ${scenario} 0 - $<TARGET_FILE:qdc> --quiet ${scripts}/session-hubs.qdc)
    set_tests_properties(cli_favourites cli_recent_hubs cli_session_hubs PROPERTIES TIMEOUT 120)

    add_test(NAME cli_interactive
        COMMAND ${scripts}/interactive.sh $<TARGET_FILE:qdc>)
    set_tests_properties(cli_interactive PROPERTIES TIMEOUT 120)

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

    # The same scenarios with each core in a qdc agent of its own, driven by a controller.
    set(remote ${scripts}/remote.sh)
    foreach(name two-cores-chat download-active)
        string(REPLACE "-" "_" test ${name})
        add_test(NAME cli_remote_${test}
            COMMAND ${remote} 0 - $<TARGET_FILE:qdc> ${scripts}/${name}.qdc a b)
    endforeach()

    add_test(NAME cli_remote_fails
        COMMAND ${remote} 1 "remote-fails.qdc:10: @b chat nohub hello: core 'b' has no hub 'nohub' open"
                $<TARGET_FILE:qdc> ${scripts}/remote-fails.qdc a b)

    add_test(NAME cli_remote_agent_dies
        COMMAND ${remote} 1 "remote-agent-dies.qdc:13: @a wait UserJoined nick=nobody timeout=60s: the agent at"
                $<TARGET_FILE:qdc> --kill b "both cores are up" ${scripts}/remote-agent-dies.qdc a b)

    add_test(NAME cli_remote_wrong_key
        COMMAND ${remote} 1 "did not say hello"
                $<TARGET_FILE:qdc> --wrong-key ${scripts}/two-cores-chat.qdc a b)

    set_tests_properties(cli_remote_two_cores_chat cli_remote_download_active cli_remote_fails
                         cli_remote_agent_dies cli_remote_wrong_key PROPERTIES TIMEOUT 120)

    # Scenarios across rootless podman networks (tests/net/run.sh): a topology and
    # its script, skipped where podman or its networking is not there. ctest -L net.
    set(net ${scripts}/net)
    # DROP's own server, for the topologies with a reachability server on the wan:
    # cd drop && CGO_ENABLED=0 go build ./cmd/dropd. Those tests skip without it.
    find_program(QUICKDC_DROPD dropd DOC "DROP's server, dropd, for the net tests")
    # test|topology|script|definitions (comma separated)
    foreach(spec
            "public_hub_two_lans|public-hub-two-lans|public-hub-two-lans|"
            "passive_traversal|public-hub-two-lans|passive-traversal|ENCRYPT=0,SECURE=false,KEY=ssl/quickdc.key"
            "passive_traversal_tls|public-hub-two-lans|passive-traversal|ENCRYPT=1,SECURE=true,KEY=spare"
            "traversal_symmetric|public-hub-symmetric|traversal-fails|"
            "traversal_one_symmetric|public-hub-one-symmetric|traversal-fails|"
            "reach_upnp|upnp|reach-upnp|"
            "reach_occupied|occupied|reach-occupied|"
            "ipv6_dual_stack|ipv6-dual|ipv6-dual|"
            "ipv6_only|ipv6-only|ipv6-only|"
            "ipv6_traversal|ipv6-closed|ipv6-traversal|"
            "lossy_link|lossy|lossy|"
            "link_cut|link-cut|link-cut|"
            "router_restart|link-reboot|link-cut|")
        string(REPLACE "|" ";" parts "${spec}")
        list(GET parts 0 test)
        list(GET parts 1 topology)
        list(GET parts 2 script)
        list(GET parts 3 definitions)
        set(defines)
        if(definitions)
            string(REPLACE "," ";" definitions "${definitions}")
            foreach(definition ${definitions})
                list(APPEND defines -D ${definition})
            endforeach()
        endif()
        add_test(NAME cli_net_${test}
            COMMAND ${net}/run.sh $<TARGET_FILE:qdc> ${net}/topologies/${topology}.topo
                    ${net}/${script}.qdc --quiet ${defines})
        set_tests_properties(cli_net_${test} PROPERTIES
            LABELS net SKIP_RETURN_CODE 77 TIMEOUT 300
            ENVIRONMENT "DROPD=${QUICKDC_DROPD}")
    endforeach()

    # Each core makes a 3072-bit certificate on its first run, which is slow to
    # generate on a loaded machine.
    set_tests_properties(cli_two_cores_chat cli_wait_times_out PROPERTIES TIMEOUT 120)
endif()
