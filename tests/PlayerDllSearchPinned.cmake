# Fails unless the player's load-time imports resolve from System32 only, and
# the neural helper's do not (P1.29).
#
#   cmake -DDUMPBIN=<dumpbin.exe> -DPINNED=<player exe> -DUNPINNED=<helper exe> -P PlayerDllSearchPinned.cmake
#
# /DEPENDENTLOADFLAG:0x800 (LOAD_LIBRARY_SEARCH_SYSTEM32) is written into the
# load configuration directory, which is where the loader reads it before any
# code in the image runs; SetDefaultDllDirectories is too late for the static
# imports. The helper must keep the default search, because its dxgi.dll is
# the ReShade proxy beside it.

if(NOT DUMPBIN OR NOT EXISTS "${DUMPBIN}")
    message(FATAL_ERROR "dumpbin.exe was not found ('${DUMPBIN}'); it ships beside cl.exe in every MSVC toolset.")
endif()

function(read_dependent_load_flag binary result)
    if(NOT EXISTS "${binary}")
        message(FATAL_ERROR "Binary is missing: ${binary}")
    endif()
    execute_process(COMMAND "${DUMPBIN}" /nologo /loadconfig "${binary}"
        OUTPUT_VARIABLE listing ERROR_VARIABLE listing_error RESULT_VARIABLE code)
    if(NOT code EQUAL 0)
        message(FATAL_ERROR "dumpbin /loadconfig failed on ${binary} (${code}): ${listing_error}")
    endif()
    string(REGEX MATCH "([0-9A-Fa-f]+) Dependent Load Flag" matched "${listing}")
    if(NOT matched)
        message(FATAL_ERROR "dumpbin printed no Dependent Load Flag for ${binary}:\n${listing}")
    endif()
    set(${result} "${CMAKE_MATCH_1}" PARENT_SCOPE)
endfunction()

read_dependent_load_flag("${PINNED}" pinned)
read_dependent_load_flag("${UNPINNED}" unpinned)
math(EXPR pinned_value "0x${pinned}")
math(EXPR unpinned_value "0x${unpinned}")
get_filename_component(pinned_name "${PINNED}" NAME)
get_filename_component(unpinned_name "${UNPINNED}" NAME)
if(NOT pinned_value EQUAL 2048)
    message(FATAL_ERROR "${pinned_name} has Dependent Load Flag ${pinned}; it must be 0800 (System32 only).")
endif()
if(NOT unpinned_value EQUAL 0)
    message(FATAL_ERROR "${unpinned_name} has Dependent Load Flag ${unpinned}; it must keep the default search for its proxy dxgi.dll.")
endif()
message(STATUS "${pinned_name}: Dependent Load Flag 0800; ${unpinned_name}: 0000.")
