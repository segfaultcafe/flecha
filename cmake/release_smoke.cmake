foreach(variable IN ITEMS FLECHA_COMPILER LONGBOW OUTPUT_ROOT)
    if(NOT DEFINED ${variable})
        message(FATAL_ERROR "${variable} is required")
    endif()
endforeach()

if(NOT DEFINED EXECUTABLE_SUFFIX)
    set(EXECUTABLE_SUFFIX "")
endif()

if(DEFINED STANDARD_LIBRARY)
    get_filename_component(STANDARD_LIBRARY "${STANDARD_LIBRARY}" ABSOLUTE)
    if(NOT IS_DIRECTORY "${STANDARD_LIBRARY}")
        message(FATAL_ERROR
            "STANDARD_LIBRARY is not a directory: ${STANDARD_LIBRARY}")
    endif()
    set(ENV{FLECHA_STANDARD_LIBRARY} "${STANDARD_LIBRARY}")
endif()

execute_process(
    COMMAND "${FLECHA_COMPILER}" --version
    RESULT_VARIABLE versionResult
    OUTPUT_VARIABLE versionOutput
    ERROR_VARIABLE versionError
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(NOT versionResult EQUAL 0 OR NOT versionOutput MATCHES
   "^flecha [0-9]+\\.[0-9]+\\.[0-9]+(-([0-9]+|dev)-g[0-9a-f]+(-dirty)?)?$")
    message(FATAL_ERROR
        "compiler version probe failed: ${versionOutput}${versionError}")
endif()
string(REGEX REPLACE "^flecha " "" expectedVersion "${versionOutput}")

execute_process(
    COMMAND "${LONGBOW}" --version
    RESULT_VARIABLE longbowVersionResult
    OUTPUT_VARIABLE longbowVersionOutput
    ERROR_VARIABLE longbowVersionError
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(NOT longbowVersionResult EQUAL 0 OR
   NOT longbowVersionOutput STREQUAL "longbow ${expectedVersion}")
    message(FATAL_ERROR
        "Longbow version probe failed: ${longbowVersionOutput}${longbowVersionError}")
endif()

file(REMOVE_RECURSE "${OUTPUT_ROOT}")
file(MAKE_DIRECTORY "${OUTPUT_ROOT}")
set(smokeSource "${OUTPUT_ROOT}/native-smoke.fl")
file(WRITE "${smokeSource}"
    "i32 add(i32 left, i32 right) { return left + right; }\n"
    "i32 main() { return add(20, 22); }\n")

execute_process(
    COMMAND "${FLECHA_COMPILER}" check "${smokeSource}"
    RESULT_VARIABLE checkResult
    ERROR_VARIABLE checkError
)
if(NOT checkResult EQUAL 0)
    message(FATAL_ERROR "compiler check failed: ${checkError}")
endif()

set(objectOutput "${OUTPUT_ROOT}/native-smoke.o")
execute_process(
    COMMAND "${FLECHA_COMPILER}" --emit-object "${smokeSource}"
        -o "${objectOutput}"
    RESULT_VARIABLE objectResult
    ERROR_VARIABLE objectError
)
if(NOT objectResult EQUAL 0 OR NOT EXISTS "${objectOutput}")
    message(FATAL_ERROR "object emission failed: ${objectError}")
endif()

set(nativeOutput "${OUTPUT_ROOT}/native-smoke${EXECUTABLE_SUFFIX}")
execute_process(
    COMMAND "${FLECHA_COMPILER}" build
        "${smokeSource}" -o "${nativeOutput}"
    RESULT_VARIABLE compileResult
    ERROR_VARIABLE compileError
)
if(NOT compileResult EQUAL 0)
    message(FATAL_ERROR "native smoke compilation failed: ${compileError}")
endif()
execute_process(COMMAND "${nativeOutput}" RESULT_VARIABLE nativeResult)
if(NOT nativeResult EQUAL 42)
    message(FATAL_ERROR
        "native smoke returned ${nativeResult} instead of 42")
endif()

set(callableRoot "${OUTPUT_ROOT}/callable-smoke")
file(MAKE_DIRECTORY "${callableRoot}")
file(WRITE "${callableRoot}/pipeline.fl"
    "module pipeline;\n"
    "export func Mapper(i32) => (i32);\n"
    "export unsafe func Reader() => (i32);\n"
    "export structure Pipeline { Mapper callback; };\n"
    "Pipeline Pipeline::init(Mapper callback) <- callback override {\n"
    "    return build Pipeline { self.callback = callback; };\n"
    "}\n"
    "i32 Pipeline::run(&const self, i32 value) {\n"
    "    return (self->callback)(value);\n"
    "}\n"
    "export T relay<T>(T value) <- value { return value; }\n"
    "export Pipeline offset(i32 amount) {\n"
    "    Mapper callback = [copy amount](value) => { return value + amount; };\n"
    "    return Pipeline::init(callback);\n"
    "}\n"
    "export Mapper combine(i32 left, i32 right) {\n"
    "    return [copy left, copy right](value) => {\n"
    "        return value + left + right;\n"
    "    };\n"
    "}\n"
    "export unsafe Reader retain(own *i32 pointer) {\n"
    "    return unsafe [own pointer]() => { return *pointer; };\n"
    "}\n")
file(WRITE "${callableRoot}/main.fl"
    "module main;\n"
    "@imports :: root::pipeline;\n"
    "i32 main() {\n"
    "    runner := pipeline::offset(20);\n"
    "    if runner.run(2) != 22 { return 1; }\n"
    "    i32 base = 0;\n"
    "    pipeline::Mapper generic = pipeline::relay<pipeline::Mapper>(\n"
    "        [copy base](value) => { return value + base; });\n"
    "    if generic(42) != 42 { return 2; }\n"
    "    pipeline::Mapper combined = pipeline::combine(10, 12);\n"
    "    (pipeline::Mapper, i32) pair = (combined, 20);\n"
    "    callback, input := pair;\n"
    "    if callback(input) != 42 { return 3; }\n"
    "    pipeline::Mapper addTwo = pipeline::combine(1, 1);\n"
    "    pipeline::Mapper addFour = pipeline::combine(2, 2);\n"
    "    [2]pipeline::Mapper pending = [addTwo, addFour];\n"
    "    [2]pipeline::Mapper callbacks = pending;\n"
    "    if (callbacks[0])(40) != 42 { return 4; }\n"
    "    if (callbacks[1])(38) != 42 { return 5; }\n"
    "    unsafe {\n"
    "        pointer, failure := new<i32>(42);\n"
    "        if failure != none { return 6; }\n"
    "        pipeline::Reader owner = pipeline::retain(pointer);\n"
    "        if owner() != 42 { return 7; }\n"
    "    }\n"
    "    return 42;\n"
    "}\n")

execute_process(
    COMMAND "${FLECHA_COMPILER}" check "${callableRoot}/main.fl"
        --root "${callableRoot}"
    RESULT_VARIABLE callableCheckResult
    ERROR_VARIABLE callableCheckError
)
if(NOT callableCheckResult EQUAL 0)
    message(FATAL_ERROR
        "callable ABI smoke check failed: ${callableCheckError}")
endif()

set(callableOutput "${callableRoot}/callable-smoke${EXECUTABLE_SUFFIX}")
execute_process(
    COMMAND "${FLECHA_COMPILER}" build "${callableRoot}/main.fl"
        --root "${callableRoot}" -o "${callableOutput}"
    RESULT_VARIABLE callableCompileResult
    ERROR_VARIABLE callableCompileError
)
if(NOT callableCompileResult EQUAL 0)
    message(FATAL_ERROR
        "callable ABI smoke compilation failed: ${callableCompileError}")
endif()
execute_process(
    COMMAND "${callableOutput}"
    RESULT_VARIABLE callableResult
)
if(NOT callableResult EQUAL 42)
    message(FATAL_ERROR
        "callable ABI smoke returned ${callableResult} instead of 42")
endif()

execute_process(
    COMMAND "${LONGBOW}" new hello
    WORKING_DIRECTORY "${OUTPUT_ROOT}"
    RESULT_VARIABLE createResult
    OUTPUT_VARIABLE createOutput
    ERROR_VARIABLE createError
)
if(NOT createResult EQUAL 0)
    message(FATAL_ERROR
        "Longbow project creation failed: ${createOutput}${createError}")
endif()
file(WRITE "${OUTPUT_ROOT}/hello/src/main.fl"
    "module main;\n"
    "@imports :: lib::std::containers::array;\n"
    "void main() { stdoutln(\"Hello from Flecha!\"); }\n")
execute_process(
    COMMAND "${LONGBOW}" check
    WORKING_DIRECTORY "${OUTPUT_ROOT}/hello"
    RESULT_VARIABLE longbowCheckResult
    OUTPUT_VARIABLE longbowCheckOutput
    ERROR_VARIABLE longbowCheckError
)
if(NOT longbowCheckResult EQUAL 0)
    message(FATAL_ERROR
        "Longbow check failed: ${longbowCheckOutput}${longbowCheckError}")
endif()
execute_process(
    COMMAND "${LONGBOW}" build
    WORKING_DIRECTORY "${OUTPUT_ROOT}/hello"
    RESULT_VARIABLE longbowBuildResult
    OUTPUT_VARIABLE longbowBuildOutput
    ERROR_VARIABLE longbowBuildError
)
if(NOT longbowBuildResult EQUAL 0)
    message(FATAL_ERROR
        "Longbow build failed: ${longbowBuildOutput}${longbowBuildError}")
endif()
execute_process(
    COMMAND "${LONGBOW}" run
    WORKING_DIRECTORY "${OUTPUT_ROOT}/hello"
    RESULT_VARIABLE runResult
    OUTPUT_VARIABLE runOutput
    ERROR_VARIABLE runError
)
string(REPLACE "\r\n" "\n" runOutput "${runOutput}")
if(NOT runResult EQUAL 0 OR NOT runOutput STREQUAL "Hello from Flecha!\n")
    message(FATAL_ERROR
        "Longbow smoke failed: ${runOutput}${runError}")
endif()
