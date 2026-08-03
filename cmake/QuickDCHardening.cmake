#
## Compiler and linker hardening for QuickDC
## Copyright (c) 2001-2010, Jan Vidar Krey
#
#
# Every flag here is probed rather than assumed: -Wl,-z,* is ELF only,
# -fcf-protection is x86 only, -mbranch-protection is AArch64 only, and the
# standard library hardening macros are spelled differently by libstdc++ and
# libc++. A flag the toolchain does not understand is left out instead of
# failing the configure.
#
# Call quickdc_harden(<target>) on each target that should get them.

include(CheckCXXCompilerFlag)
include(CheckCXXSourceCompiles)
if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.18)
    include(CheckLinkerFlag)
endif()

# Informational warnings that have too many existing instances to be useful by
# default: 386 C-style casts and 104 raw char* members between them would bury
# the ones worth reading. Available for a sweep, off for an ordinary build.
option(QUICKDC_PEDANTIC
    "Add the noisy informational warnings (conversion, old-style-cast, shadow)" OFF)

set(QUICKDC_HARDEN_COMPILE "")
set(QUICKDC_HARDEN_LINK "")
set(QUICKDC_HARDEN_DEFINES "")

function(_qdc_try_compile flag var)
    string(MAKE_C_IDENTIFIER "QDC_HAS${flag}" cv)

    # -Werror=unused-command-line-argument as well as accepting the flag: clang
    # takes some codegen flags for targets that cannot use them and then says so
    # once per translation unit, which is noise rather than protection. Asking
    # for the complaint as an error is what tells the two cases apart.
    set(CMAKE_REQUIRED_FLAGS "-Werror=unused-command-line-argument")
    check_cxx_compiler_flag("${flag}" ${cv})
    unset(CMAKE_REQUIRED_FLAGS)

    if(${cv})
        set(${var} ${${var}} "${flag}" PARENT_SCOPE)
    endif()
endfunction()

function(_qdc_try_link flag var)
    string(MAKE_C_IDENTIFIER "QDC_HASLINK${flag}" cv)
    if(COMMAND check_linker_flag)
        check_linker_flag(CXX "${flag}" ${cv})
    else()
        set(CMAKE_REQUIRED_LINK_OPTIONS "${flag}")
        check_cxx_source_compiles("int main(){return 0;}" ${cv})
        unset(CMAKE_REQUIRED_LINK_OPTIONS)
    endif()
    if(${cv})
        set(${var} ${${var}} "${flag}" PARENT_SCOPE)
    endif()
endfunction()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    list(APPEND QUICKDC_HARDEN_COMPILE -Wall -Wextra)

    # A curated -Werror set. Each of these is either a class of bug this
    # codebase has already shipped - a %s given something that is not a string,
    # a switch that fell through to read an uninitialised pointer - or one that
    # cannot be anything but a mistake. Wholesale -Werror would instead fail on
    # -Wunused-parameter from the event handler stubs, which is not the point.
    #
    # suggest-override is here rather than in the pedantic set below because the
    # tree is clean of it and the cost of letting it back in is silent: QuickDC
    # implements samurai's SocketEventHandler, TimerListener and friends by
    # declaring matching signatures, and those base methods are defaulted rather
    # than pure. A parameter gaining a const on samurai's side therefore produces
    # a new, non-overriding function, a build that still succeeds, and a session
    # that quietly stops receiving events. Nothing else in the project would
    # notice.
    foreach(w format format-security format-nonliteral return-type nonnull
              uninitialized array-bounds sizeof-pointer-memaccess
              memset-transposed-args implicit-fallthrough cast-align
              delete-non-virtual-dtor vla tautological-bitwise-compare
              suggest-override)
        _qdc_try_compile("-Werror=${w}" QUICKDC_HARDEN_COMPILE)
    endforeach()

    if(QUICKDC_PEDANTIC)
        foreach(w pedantic conversion sign-conversion old-style-cast shadow
                  double-promotion)
            _qdc_try_compile("-W${w}" QUICKDC_HARDEN_COMPILE)
        endforeach()
    endif()

    _qdc_try_compile(-fstack-protector-strong        QUICKDC_HARDEN_COMPILE)
    _qdc_try_compile(-fstack-clash-protection        QUICKDC_HARDEN_COMPILE)
    _qdc_try_compile(-fno-strict-aliasing            QUICKDC_HARDEN_COMPILE)
    _qdc_try_compile(-fno-delete-null-pointer-checks QUICKDC_HARDEN_COMPILE)
    _qdc_try_compile(-ftrivial-auto-var-init=zero    QUICKDC_HARDEN_COMPILE)

    if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
        _qdc_try_compile(-mbranch-protection=standard QUICKDC_HARDEN_COMPILE)
    else()
        _qdc_try_compile(-fcf-protection=full QUICKDC_HARDEN_COMPILE)
    endif()

    # _FORTIFY_SOURCE needs optimisation to do anything, and warns if it is
    # already defined - hence the -U, and hence being a compile option rather
    # than a definition, so that the two reach the command line in this order.
    # 3 where it is understood, 2 otherwise.
    set(CMAKE_REQUIRED_FLAGS "-O2 -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=3 -Werror")
    check_cxx_source_compiles("int main(){return 0;}" QDC_HAS_FORTIFY_3)
    unset(CMAKE_REQUIRED_FLAGS)

    if(QDC_HAS_FORTIFY_3)
        set(_qdc_fortify 3)
    else()
        set(_qdc_fortify 2)
    endif()

    list(APPEND QUICKDC_HARDEN_COMPILE
        "$<$<NOT:$<CONFIG:Debug>>:-U_FORTIFY_SOURCE>"
        "$<$<NOT:$<CONFIG:Debug>>:-D_FORTIFY_SOURCE=${_qdc_fortify}>")

    # Bounds checks on operator[] and iterators - cheap next to parsing a
    # network protocol, and it turns an over-read into a clean abort. The two
    # standard libraries spell it differently and neither accepts the other's
    # spelling, so both are probed and only what compiles is used.
    set(CMAKE_REQUIRED_FLAGS "-D_GLIBCXX_ASSERTIONS")
    check_cxx_source_compiles("#include <vector>\nint main(){std::vector<int> v{1};return v[0]-1;}"
        QDC_HAS_GLIBCXX_ASSERTIONS)
    unset(CMAKE_REQUIRED_FLAGS)
    if(QDC_HAS_GLIBCXX_ASSERTIONS)
        list(APPEND QUICKDC_HARDEN_DEFINES _GLIBCXX_ASSERTIONS)
    endif()

    set(CMAKE_REQUIRED_FLAGS "-D_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_EXTENSIVE")
    check_cxx_source_compiles("#include <vector>\nint main(){std::vector<int> v{1};return v[0]-1;}"
        QDC_HAS_LIBCPP_HARDENING)
    unset(CMAKE_REQUIRED_FLAGS)
    if(QDC_HAS_LIBCPP_HARDENING)
        list(APPEND QUICKDC_HARDEN_DEFINES
            _LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_EXTENSIVE)
    endif()

    _qdc_try_link("-Wl,-z,relro"         QUICKDC_HARDEN_LINK)
    _qdc_try_link("-Wl,-z,now"           QUICKDC_HARDEN_LINK)
    _qdc_try_link("-Wl,-z,noexecstack"   QUICKDC_HARDEN_LINK)
    _qdc_try_link("-Wl,-z,separate-code" QUICKDC_HARDEN_LINK)
endif()

function(quickdc_harden target)
    get_target_property(_type ${target} TYPE)

    target_compile_options(${target} PRIVATE ${QUICKDC_HARDEN_COMPILE})
    target_compile_definitions(${target} PRIVATE ${QUICKDC_HARDEN_DEFINES})

    set_target_properties(${target} PROPERTIES
        POSITION_INDEPENDENT_CODE ON
        C_VISIBILITY_PRESET hidden
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON)

    if(_type STREQUAL "EXECUTABLE")
        target_link_options(${target} PRIVATE ${QUICKDC_HARDEN_LINK})
    endif()
endfunction()
