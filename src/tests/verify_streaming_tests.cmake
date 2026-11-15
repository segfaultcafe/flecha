get_filename_component(source_root "${FLECHA_SOURCE}" DIRECTORY)
foreach(mode verbose_success verbose_failure quiet_failure)
    set(log "${FLECHA_OUTPUT}-${mode}.log")
    set(options)
    set(environment "FLECHA_OUTPUT_LOG=${log}")
    if(mode MATCHES "^verbose")
        list(APPEND options --verbose)
    endif()
    if(mode MATCHES "failure$")
        list(APPEND environment FLECHA_OUTPUT_FAIL=1)
        set(expected "FAIL [1/2] tests passed, but [1/2] failed")
    else()
        set(expected "OK [2/2] tests passed")
    endif()
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env ${environment}
            "${FLECHA_COMPILER}" test "${FLECHA_SOURCE}"
            --root "${source_root}" --lib "std=${FLECHA_STD}" --link "${FLECHA_PROBE}" ${options}
        RESULT_VARIABLE status OUTPUT_FILE "${log}" ERROR_VARIABLE error
    )
    file(READ "${log}" output)
    string(FIND "${output}" "${expected}" found)
    if(found EQUAL -1 OR
       (mode MATCHES "failure$" AND NOT status EQUAL 1) OR
       (mode MATCHES "success$" AND NOT status EQUAL 0))
        message(FATAL_ERROR "${mode}: status ${status}\n${output}${error}")
    endif()
endforeach()
