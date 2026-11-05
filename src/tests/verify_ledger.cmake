foreach(variable IN ITEMS LONGBOW FLECHA_COMPILER FLECHA_STD FIXTURE TEST_ROOT)
    if(NOT DEFINED ${variable})
        message(FATAL_ERROR "${variable} is required")
    endif()
endforeach()

file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${TEST_ROOT}/project" "${TEST_ROOT}/relocated")
file(COPY "${FIXTURE}/src" DESTINATION "${TEST_ROOT}/project")
file(WRITE "${TEST_ROOT}/project/longbow.toml" [=[
[package]
name = "ledger"
version = "0.1.0"
kind = "program"

[dependencies]
]=])

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
        "FLECHA_COMPILER=${FLECHA_COMPILER}"
        "FLECHA_STANDARD_LIBRARY=${FLECHA_STD}"
        "${LONGBOW}" test
    WORKING_DIRECTORY "${TEST_ROOT}/project"
    RESULT_VARIABLE testResult
    OUTPUT_VARIABLE testOutput
    ERROR_VARIABLE testError)
if(NOT testResult EQUAL 0)
    message(FATAL_ERROR "ledger golden tests failed: ${testOutput}${testError}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
        "FLECHA_COMPILER=${FLECHA_COMPILER}"
        "FLECHA_STANDARD_LIBRARY=${FLECHA_STD}"
        "${LONGBOW}" build
    WORKING_DIRECTORY "${TEST_ROOT}/project"
    RESULT_VARIABLE buildResult
    OUTPUT_VARIABLE buildOutput
    ERROR_VARIABLE buildError)
if(NOT buildResult EQUAL 0)
    message(FATAL_ERROR "ledger build failed: ${buildOutput}${buildError}")
endif()

if(WIN32)
    set(executable "${TEST_ROOT}/project/target/ledger.exe")
else()
    set(executable "${TEST_ROOT}/project/target/ledger")
endif()
if(NOT EXISTS "${executable}")
    message(FATAL_ERROR "ledger executable was not produced")
endif()

function(run_ledger expected)
    execute_process(
        COMMAND "${executable}" ${ARGN}
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE errors)
    string(REPLACE "\r\n" "\n" output "${output}")
    string(REPLACE "\r\n" "\n" errors "${errors}")
    if(NOT result EQUAL expected)
        message(FATAL_ERROR
            "ledger ${ARGN} returned ${result}, expected ${expected}: "
            "${output}${errors}")
    endif()
    set(last_output "${output}" PARENT_SCOPE)
    set(last_errors "${errors}" PARENT_SCOPE)
endfunction()

set(alphaDigest
    "aba5ed27f8ee302525deccc60adfb950fd3d71699082bb3b5c0bcad6a36ec139")
set(betaDigest
    "d4298ba679357d6f92c5ea1da21f8895a3e6d514b35e51b3e68081744d9f6b4a")
set(expectedFileHash
    "8597adac0ae2a0efee59c082eb1ea4e68fa5752d2ab30bfdfc7d5b6743a95847")

set(ledger "${TEST_ROOT}/chain.fld")
run_ledger(0 create "${ledger}")
if(NOT last_output STREQUAL "created: 0 blocks\n")
    message(FATAL_ERROR "unexpected create output: ${last_output}")
endif()
run_ledger(0 append "${ledger}" alpha)
if(NOT last_output STREQUAL "appended: 0 ${alphaDigest}\n")
    message(FATAL_ERROR "unexpected genesis output: ${last_output}")
endif()
run_ledger(0 append "${ledger}" beta)
if(NOT last_output STREQUAL "appended: 1 ${betaDigest}\n")
    message(FATAL_ERROR "unexpected append output: ${last_output}")
endif()
run_ledger(0 verify "${ledger}")
if(NOT last_output STREQUAL "verified: 2 blocks ${betaDigest}\n")
    message(FATAL_ERROR "unexpected verify output: ${last_output}")
endif()
run_ledger(0 list "${ledger}")
if(NOT last_output STREQUAL
   "0 ${alphaDigest} 616c706861\n1 ${betaDigest} 62657461\n")
    message(FATAL_ERROR "unexpected list output: ${last_output}")
endif()

file(SHA256 "${ledger}" ledgerHash)
if(NOT ledgerHash STREQUAL expectedFileHash)
    message(FATAL_ERROR
        "canonical ledger hash ${ledgerHash} did not match ${expectedFileHash}")
endif()

set(relocated "${TEST_ROOT}/relocated/chain.fld")
run_ledger(0 create "${relocated}")
run_ledger(0 append "${relocated}" alpha)
run_ledger(0 append "${relocated}" beta)
file(SHA256 "${relocated}" relocatedHash)
if(NOT relocatedHash STREQUAL ledgerHash)
    message(FATAL_ERROR "identical relocated ledgers produced different bytes")
endif()

# Creating over an existing chain fails without changing a byte.
run_ledger(3 create "${ledger}")
file(SHA256 "${ledger}" preservedHash)
if(NOT preservedHash STREQUAL ledgerHash)
    message(FATAL_ERROR "failed create changed the existing ledger")
endif()

file(WRITE "${TEST_ROOT}/malformed.fld" "not-a-ledger")
run_ledger(4 verify "${TEST_ROOT}/malformed.fld")
if(NOT last_errors STREQUAL "ledger: invalid ledger format\n")
    message(FATAL_ERROR "unexpected malformed diagnostic: ${last_errors}")
endif()

run_ledger(3 verify "${TEST_ROOT}/missing.fld")

