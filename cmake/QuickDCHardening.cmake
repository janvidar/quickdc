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
include(CheckCXXSourceRuns)
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

#
# A codegen flag that the compiler accepts, and that leaves an exception thrown
# in one translation unit catchable in another.
#
# -mbranch-protection=standard is the case this exists for. It is accepted, it
# builds, every test passes - and on Apple clang 21 for arm64 its pac-ret half
# signs return addresses in a way the unwinder does not follow, so a throw
# crossing a function boundary reaches no handler at all and the process
# terminates. Not even catch(...) sees it. bti on its own is fine.
#
# That is not a theoretical loss: hublist.cpp catches std::bad_alloc so that a
# list too large to hold is refused rather than taking the client down with it,
# and the list arrives over the network. A flag that quietly turns that catch
# into a terminate() is worse than not hardening at all.
#
# The throw is in a separate function marked noinline, because the point is to
# unwind through a frame rather than to throw and catch in one.
#
function(_qdc_try_compile_unwinds flag var)
    string(MAKE_C_IDENTIFIER "QDC_UNWINDS${flag}" cv)

    if(CMAKE_CROSSCOMPILING)
        # Nothing can be run, so the question cannot be answered. Left out
        # rather than assumed good: a build that cannot check is the build that
        # would ship the broken case.
        message(STATUS "Cross-compiling: leaving out ${flag}, unwinding unverifiable")
        return()
    endif()

    set(CMAKE_REQUIRED_FLAGS "${flag} -Werror=unused-command-line-argument")
    check_cxx_source_runs("
        struct Boom { int code; };
        __attribute__((noinline)) static void deep() { throw Boom{42}; }
        int main() {
            try { deep(); }
            catch (const Boom& b) { return b.code == 42 ? 0 : 1; }
            catch (...) { return 2; }
            return 3;
        }" ${cv})
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
    # The two override warnings are here rather than in the pedantic set below
    # because the tree is clean of both and the cost of letting one back in is
    # silent: QuickDC implements samurai's SocketEventHandler, TimerListener and
    # friends by declaring matching signatures, and those base methods are
    # defaulted rather than pure. A parameter gaining a const on samurai's side
    # therefore produces a new, non-overriding function, a build that still
    # succeeds, and a session that quietly stops receiving events.
    #
    # Both are needed, and they are not interchangeable. Clang splits the cases:
    # suggest-override fires when nothing in the class is marked, and
    # inconsistent-missing-override when some are and one is not - which is the
    # regression case now that every override in the tree carries the keyword.
    # GCC has only suggest-override and uses it for both, so probing sorts out
    # which of the two this compiler understands.
    foreach(w format format-security format-nonliteral return-type nonnull
              uninitialized array-bounds sizeof-pointer-memaccess
              memset-transposed-args implicit-fallthrough cast-align
              delete-non-virtual-dtor vla tautological-bitwise-compare
              suggest-override inconsistent-missing-override)
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
        # standard is bti plus pac-ret. Both are probed for unwinding rather
        # than only for acceptance, and bti alone is the fallback when pac-ret
        # is the half that breaks it - which keeps the protection that works.
        _qdc_try_compile_unwinds(-mbranch-protection=standard QUICKDC_HARDEN_COMPILE)
        if(NOT "-mbranch-protection=standard" IN_LIST QUICKDC_HARDEN_COMPILE)
            _qdc_try_compile_unwinds(-mbranch-protection=bti QUICKDC_HARDEN_COMPILE)
        endif()
    else()
        _qdc_try_compile_unwinds(-fcf-protection=full QUICKDC_HARDEN_COMPILE)
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
