set(bridgeSource "${FLECHA_OUTPUT}_bridge.cpp")
set(bridgeObject "${FLECHA_OUTPUT}_bridge.o")
file(WRITE "${bridgeSource}"
    "#include <cstddef>\n#include <cstdint>\nextern \"C\" {\nstd::int32_t native_add(std::int32_t left, std::int32_t right) { return left + right; }\nstd::int32_t native_widen_i8(std::int8_t value) { return value; }\nstd::int32_t native_widen_i16(std::int16_t value) { return value; }\nstd::int8_t native_return_i8() { return -1; }\nbool native_not(bool value) { return !value; }\nstd::uint8_t native_char(std::uint8_t value) { return value; }\nbool native_is_hi_mom(const std::uint8_t* value, std::size_t length) {\n    const std::uint8_t expected[] = {'h', 'i', ' ', 'm', 'o', 'm'};\n    if (length != sizeof(expected)) return false;\n    for (std::size_t i = 0; i < length; ++i)\n        if (value[i] != expected[i]) return false;\n    return true;\n}\nvoid native_increment(std::int32_t* value) { ++*value; }\n}\n")
execute_process(
    COMMAND "${FLECHA_LINKER}" -O2 -c "${bridgeSource}" -o "${bridgeObject}"
    RESULT_VARIABLE bridgeResult ERROR_VARIABLE bridgeError TIMEOUT 30
)
if(NOT bridgeResult EQUAL 0)
    message(FATAL_ERROR "native bridge compilation failed: ${bridgeError}")
endif()

set(source "${FLECHA_OUTPUT}_main.fl")
file(WRITE "${source}"
    "extern i32 native_add(i32 left, i32 right);\nextern i32 native_widen_i8(i8 value);\nextern i32 native_widen_i16(i16 value);\nextern i8 native_return_i8();\nextern bool native_not(bool value);\nextern char native_char(char value);\nextern unsafe bool native_is_hi_mom(*const u8 value, usize length);\nextern unsafe void native_increment(*i32 value);\n\ni32 main() {\n    i8 negative8 = (-1).cast<i8>;\n    i16 negative16 = (-1).cast<i16>;\n    if native_widen_i8(negative8) != -1 or native_widen_i16(negative16) != -1 {\n        return 2;\n    }\n    if native_return_i8() != negative8 {\n        return 3;\n    }\n    if native_not(true) or native_char('F') != 'F' {\n        return 1;\n    }\n    stringv message = \"hi mom\";\n    unsafe {\n        if !native_is_hi_mom(message.data(), message.len().cast<usize>) {\n            return 4;\n        }\n    }\n    i32 result = native_add(20, 21);\n    unsafe {\n        native_increment(&raw result);\n    }\n    return result;\n}\n")
execute_process(
    COMMAND "${FLECHA_COMPILER}" build "${source}" -o "${FLECHA_OUTPUT}"
        --link "${bridgeObject}"
    RESULT_VARIABLE compileResult ERROR_VARIABLE compileError TIMEOUT 30
)
if(NOT compileResult EQUAL 0)
    message(FATAL_ERROR "extern executable compilation failed: ${compileError}")
endif()
execute_process(COMMAND "${FLECHA_OUTPUT}" RESULT_VARIABLE runResult TIMEOUT 10)
if(NOT runResult EQUAL 42)
    message(FATAL_ERROR "extern executable returned ${runResult}, expected 42")
endif()

# The conventional output-adjacent object name may itself be a native input.
# Building must neither replace that input nor feed the generated object twice.
set(collisionOutput "${FLECHA_OUTPUT}_collision")
set(collisionObject "${collisionOutput}.o")
configure_file("${bridgeObject}" "${collisionObject}" COPYONLY)
file(SHA256 "${collisionObject}" collisionHashBefore)
execute_process(
    COMMAND "${FLECHA_COMPILER}" build "${source}" -o "${collisionOutput}"
        --link "${collisionObject}"
    RESULT_VARIABLE collisionCompileResult
    ERROR_VARIABLE collisionCompileError
    TIMEOUT 30
)
if(NOT collisionCompileResult EQUAL 0)
    message(FATAL_ERROR
        "extern collision build failed: ${collisionCompileError}")
endif()
file(SHA256 "${collisionObject}" collisionHashAfter)
if(NOT collisionHashBefore STREQUAL collisionHashAfter)
    message(FATAL_ERROR "native link input was overwritten during build")
endif()
execute_process(
    COMMAND "${collisionOutput}" RESULT_VARIABLE collisionRunResult TIMEOUT 10)
if(NOT collisionRunResult EQUAL 42)
    message(FATAL_ERROR
        "extern collision executable returned ${collisionRunResult}, expected 42")
endif()

set(project "${FLECHA_OUTPUT}_longbow")
file(MAKE_DIRECTORY "${project}/src" "${project}/native")
file(COPY "${bridgeObject}" DESTINATION "${project}/native")
get_filename_component(bridgeName "${bridgeObject}" NAME)
file(WRITE "${project}/longbow.toml"
    "[package]\nname = \"extern_app\"\nversion = \"0.1.0\"\nkind = \"program\"\n\n[dependencies]\n\n[native]\nbridge = { path = \"native/${bridgeName}\" }\n")
get_filename_component(sourceName "${source}" NAME)
file(COPY "${source}" DESTINATION "${project}/src")
file(RENAME "${project}/src/${sourceName}" "${project}/src/main.fl")
execute_process(
    COMMAND "${LONGBOW}" run
    WORKING_DIRECTORY "${project}"
    RESULT_VARIABLE longbowResult ERROR_VARIABLE longbowError TIMEOUT 30
)
if(NOT longbowResult EQUAL 42)
    message(FATAL_ERROR
        "longbow extern program returned ${longbowResult}, expected 42: ${longbowError}")
endif()
