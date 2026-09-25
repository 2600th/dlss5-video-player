# Fails if a shipped executable imports a Visual C++ runtime DLL.
#
#   cmake -DDUMPBIN=<dumpbin.exe> "-DBINARIES=<exe>|<exe>" -P ShippedRuntimeImports.cmake
#
# The binaries are separated by '|' rather than ';' so the list survives
# add_test and CTestTestfile.cmake as one argument.
#
# Both executables link the C and C++ runtimes statically (CMakeLists.txt,
# CMAKE_MSVC_RUNTIME_LIBRARY), because neither package carries the Visual C++
# Redistributable: an import of MSVCP140, VCRUNTIME140 or their siblings means
# the player does not start on a machine without it, and crashes at its first
# log line on one with a redistributable older than the toolset. The UCRT's
# api-ms-win-crt-* forwarders and ucrtbase.dll are part of Windows 10, but they
# appear only when the runtime is the DLL one, so they are refused as well: an
# import of any of them says the static link did not happen.

if(NOT DUMPBIN OR NOT EXISTS "${DUMPBIN}")
    message(FATAL_ERROR "dumpbin.exe was not found ('${DUMPBIN}'); it ships beside cl.exe in every MSVC toolset.")
endif()
string(REPLACE "|" ";" BINARIES "${BINARIES}")
if(NOT BINARIES)
    message(FATAL_ERROR "No binaries to check.")
endif()

set(runtime_pattern "^(msvcp|vcruntime|concrt|vccorlib|vcomp|vcamp|mfc)[0-9].*\\.dll$|^ucrtbased?\\.dll$|^api-ms-win-crt-.*\\.dll$")
set(failures "")
foreach(binary IN LISTS BINARIES)
    if(NOT EXISTS "${binary}")
        message(FATAL_ERROR "Shipped binary is missing: ${binary}")
    endif()
    execute_process(COMMAND "${DUMPBIN}" /nologo /dependents "${binary}"
        OUTPUT_VARIABLE listing ERROR_VARIABLE listing_error RESULT_VARIABLE result)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "dumpbin /dependents failed on ${binary} (${result}): ${listing_error}")
    endif()
    # dumpbin lists each dependency on its own indented line, the statically
    # bound ones first and then any delay-loaded ones; both count, since a
    # delay-loaded runtime fails the same way on first use.
    string(REPLACE "\r" "" listing "${listing}")
    string(REPLACE "\n" ";" lines "${listing}")
    set(dependencies "")
    foreach(line IN LISTS lines)
        string(STRIP "${line}" name)
        string(TOLOWER "${name}" name)
        if(name MATCHES "^[^ ]+\\.dll$")
            list(APPEND dependencies "${name}")
        endif()
    endforeach()
    if(NOT dependencies)
        message(FATAL_ERROR "dumpbin listed no DLL dependencies for ${binary}, so its output was not understood:\n${listing}")
    endif()
    set(runtime_imports "")
    foreach(dependency IN LISTS dependencies)
        if(dependency MATCHES "${runtime_pattern}")
            list(APPEND runtime_imports "${dependency}")
        endif()
    endforeach()
    get_filename_component(binary_name "${binary}" NAME)
    if(runtime_imports)
        list(JOIN runtime_imports ", " joined)
        string(APPEND failures "\n  ${binary_name} imports ${joined}")
    else()
        list(LENGTH dependencies count)
        message(STATUS "${binary_name}: ${count} DLL dependencies, none of them a C or C++ runtime.")
    endif()
endforeach()

if(failures)
    message(FATAL_ERROR "A shipped executable imports the Visual C++ runtime, which no package carries:${failures}")
endif()
