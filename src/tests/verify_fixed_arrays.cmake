execute_process(
    COMMAND "${FLECHA_COMPILER}" --emit-object "${FLECHA_SOURCE}" -o "${FLECHA_OUTPUT}.o"
    RESULT_VARIABLE compileResult ERROR_VARIABLE compileError TIMEOUT 30
)
if(NOT compileResult EQUAL 0)
    message(FATAL_ERROR "native compilation failed: ${compileError}")
endif()
set(sanitizerFlags)
if(FLECHA_SANITIZERS)
    list(APPEND sanitizerFlags -fsanitize=address,undefined)
endif()
execute_process(
    COMMAND "${FLECHA_LINKER}" "${FLECHA_HARNESS}" "${FLECHA_OUTPUT}.o"
        "${FLECHA_RUNTIME}" ${sanitizerFlags} -o "${FLECHA_OUTPUT}"
    RESULT_VARIABLE linkResult ERROR_VARIABLE linkError TIMEOUT 30
)
if(NOT linkResult EQUAL 0)
    message(FATAL_ERROR "native link failed: ${linkError}")
endif()
execute_process(
    COMMAND "${FLECHA_OUTPUT}"
    RESULT_VARIABLE runResult OUTPUT_VARIABLE actualOutput ERROR_VARIABLE runError TIMEOUT 10
)
if(NOT runResult EQUAL 42)
    message(FATAL_ERROR "native program returned ${runResult}, expected 42: ${runError}")
endif()
if(NOT actualOutput STREQUAL "")
    message(FATAL_ERROR "unexpected native output: ${actualOutput}")
endif()

# An invalid access must use the structured crash path, including unsafe
# accesses and address creation.
if(NOT DEFINED FLECHA_TRAP_MODES)
    set(FLECHA_TRAP_MODES r w b u m n s z g q)
endif()
foreach(mode IN LISTS FLECHA_TRAP_MODES)
    execute_process(COMMAND "${FLECHA_OUTPUT}" "${mode}"
        RESULT_VARIABLE trapResult OUTPUT_VARIABLE trapOutput ERROR_VARIABLE trapError TIMEOUT 10)
    if(NOT trapResult MATCHES "Subprocess aborted|^134$")
        message(FATAL_ERROR
            "bounds test ${mode} did not abort through the runtime: ${trapResult}: ${trapError}")
    endif()
    if(NOT trapOutput STREQUAL "")
        message(FATAL_ERROR "invalid store evaluated its RHS: ${trapOutput}")
    endif()
    foreach(expected IN ITEMS
            "flecha: unrecoverable crash"
            "out of bounds ("
            "length:"
            "location: ${FLECHA_SOURCE}:"
            "registers:"
            "stack trace:")
        string(FIND "${trapError}" "${expected}" position)
        if(position EQUAL -1)
            message(FATAL_ERROR
                "bounds test ${mode} is missing '${expected}'\nstderr:\n${trapError}")
        endif()
    endforeach()
    if(trapError MATCHES "Illegal instruction")
        message(FATAL_ERROR "bounds test ${mode} still raised SIGILL")
    endif()
    if(mode MATCHES "^(r|w|b)$")
        set(expectedMessage "index out of bounds (index: 2, length: 2)")
    elseif(mode STREQUAL "z")
        set(expectedMessage "index out of bounds (index: 0, length: 0)")
    elseif(mode STREQUAL "g")
        set(expectedMessage "index out of bounds (index: 3, length: 2)")
    elseif(mode STREQUAL "q")
        set(expectedMessage "index out of bounds (index: -1, length: 2)")
    elseif(mode STREQUAL "o")
        set(expectedMessage "slice bounds out of bounds (start: 2, end: 1, length: 3)")
    elseif(mode STREQUAL "e")
        set(expectedMessage "slice bounds out of bounds (start: 0, end: 4, length: 3)")
    else()
        unset(expectedMessage)
    endif()
    if(DEFINED expectedMessage)
        string(FIND "${trapError}" "${expectedMessage}" messagePosition)
        if(messagePosition EQUAL -1)
            message(FATAL_ERROR
                "bounds test ${mode} is missing '${expectedMessage}'\nstderr:\n${trapError}")
        endif()
    endif()
endforeach()
