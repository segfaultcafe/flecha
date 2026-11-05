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
else()
    get_filename_component(compilerDirectory "${FLECHA_COMPILER}" DIRECTORY)
    get_filename_component(installRoot "${compilerDirectory}" DIRECTORY)
    set(STANDARD_LIBRARY "${installRoot}/share/flecha/std/src")
    if(NOT IS_DIRECTORY "${STANDARD_LIBRARY}")
        message(FATAL_ERROR
            "installed standard library is not a directory: ${STANDARD_LIBRARY}")
    endif()
endif()
set(testSupportLibraryArguments --lib "std=${STANDARD_LIBRARY}")

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

set(testSupportSource "${OUTPUT_ROOT}/test-support-smoke.fl")
file(WRITE "${testSupportSource}"
    "module test_support_smoke;\n"
    "@imports\n"
    "    :: lib::std::io::filesystem\n"
    "    :: lib::std::tests::fixtures\n"
    "    :: lib::std::tests::benchmark\n"
    "    :: lib::std::tests::golden\n"
    "    :: lib::std::tests::testing;\n"
    "i32 main() {\n"
    "    string ownedPath = none;\n"
    "    {\n"
    "        temporary, creationError := fixtures::TemporaryDirectory::create();\n"
    "        if creationError != none { return 1; }\n"
    "        ownedPath = temporary.path().string();\n"
    "        goldenPath, pathError := temporary.childPath(\"expected.txt\");\n"
    "        if pathError != none { return 2; }\n"
    "        result := testing::UnitTest::init(\"release smoke\", false);\n"
    "        if golden::text(&result, \"expected\", goldenPath.view(),\n"
    "                \"golden update\", golden::Mode.Update) != none { return 3; }\n"
    "        if golden::text(&result, \"expected\", goldenPath.view(),\n"
    "                \"golden verify\") != none { return 4; }\n"
    "        first := fixtures::RandomData::fromSeed(144);\n"
    "        second := fixtures::RandomData::fromSeed(144);\n"
    "        if first.integer<u64>() != second.integer<u64>() { return 5; }\n"
    "        if first.seed() != 144 or first.logSeed(&result) != none { return 6; }\n"
    "        if result.assertionCount() != 2 or result.failureCount() != 0 { return 7; }\n"
    "        config, configError := benchmark::Config::create(0, 1, 2);\n"
    "        if configError != none { return 8; }\n"
    "        benchmarkResult, benchmarkError := benchmark::run(\"release.noop\",\n"
    "            config, () => (error) { return none; });\n"
    "        if benchmarkError != none or benchmarkResult.sampleCount() != 2 { return 9; }\n"
    "        machine, machineError := benchmarkResult.machineLine();\n"
    "        if machineError != none or machine.len() == 0 { return 10; }\n"
    "    }\n"
    "    present, existsError := filesystem::exists(ownedPath.view());\n"
    "    if existsError != none or present { return 11; }\n"
    "    return 42;\n"
    "}\n")

set(testSupportOutput "${OUTPUT_ROOT}/test-support-smoke${EXECUTABLE_SUFFIX}")
execute_process(
    COMMAND "${FLECHA_COMPILER}" build "${testSupportSource}"
        ${testSupportLibraryArguments} -o "${testSupportOutput}"
    RESULT_VARIABLE testSupportCompileResult
    ERROR_VARIABLE testSupportCompileError
)
if(NOT testSupportCompileResult EQUAL 0)
    message(FATAL_ERROR
        "test support smoke compilation failed: ${testSupportCompileError}")
endif()
execute_process(
    COMMAND "${testSupportOutput}"
    RESULT_VARIABLE testSupportResult
)
if(NOT testSupportResult EQUAL 42)
    message(FATAL_ERROR
        "test support smoke returned ${testSupportResult} instead of 42")
endif()

# Run the published one-million-'a' SHA-256 and SHA-512 vectors through both
# source-tree and relocated standard-library packages on every release target.
set(sha2SmokeSource "${OUTPUT_ROOT}/sha2-smoke.fl")
file(WRITE "${sha2SmokeSource}" [=[
module sha2_smoke;

@imports :: lib::std::cryptography::sha2;

bool equals([]const u8 actual, []const u8 expected) {
    if actual.len() != expected.len() { return false; }
    loop usize index = 0; index < actual.len(); index++ {
        if actual[index] != expected[index] { return false; }
    }
    return true;
}

i32 main() {
    [1000]u8 chunk = [0x61; 1000];

    sha256Hasher := sha2::Sha256::init();
    loop usize iteration = 0; iteration < 1000; iteration++ {
        if sha256Hasher.update(chunk[:]) != none { return 1; }
    }
    sha256Digest, sha256Error := sha256Hasher.finalize();
    [32]u8 sha256Expected = [
        0xcd, 0xc7, 0x6e, 0x5c, 0x99, 0x14, 0xfb, 0x92,
        0x81, 0xa1, 0xc7, 0xe2, 0x84, 0xd7, 0x3e, 0x67,
        0xf1, 0x80, 0x9a, 0x48, 0xa4, 0x97, 0x20, 0x0e,
        0x04, 0x6d, 0x39, 0xcc, 0xc7, 0x11, 0x2c, 0xd0
    ];
    if sha256Error != none or
        !equals(sha256Digest.bytes(), sha256Expected[:]) {
        return 2;
    }

    sha512Hasher := sha2::Sha512::init();
    loop usize iteration = 0; iteration < 1000; iteration++ {
        if sha512Hasher.update(chunk[:]) != none { return 3; }
    }
    sha512Digest, sha512Error := sha512Hasher.finalize();
    [64]u8 sha512Expected = [
        0xe7, 0x18, 0x48, 0x3d, 0x0c, 0xe7, 0x69, 0x64,
        0x4e, 0x2e, 0x42, 0xc7, 0xbc, 0x15, 0xb4, 0x63,
        0x8e, 0x1f, 0x98, 0xb1, 0x3b, 0x20, 0x44, 0x28,
        0x56, 0x32, 0xa8, 0x03, 0xaf, 0xa9, 0x73, 0xeb,
        0xde, 0x0f, 0xf2, 0x44, 0x87, 0x7e, 0xa6, 0x0a,
        0x4c, 0xb0, 0x43, 0x2c, 0xe5, 0x77, 0xc3, 0x1b,
        0xeb, 0x00, 0x9c, 0x5c, 0x2c, 0x49, 0xaa, 0x2e,
        0x4e, 0xad, 0xb2, 0x17, 0xad, 0x8c, 0xc0, 0x9b
    ];
    if sha512Error != none or
        !equals(sha512Digest.bytes(), sha512Expected[:]) {
        return 4;
    }

    return 42;
}
]=])

set(sha2SmokeOutput "${OUTPUT_ROOT}/sha2-smoke${EXECUTABLE_SUFFIX}")
execute_process(
    COMMAND "${FLECHA_COMPILER}" build "${sha2SmokeSource}"
        ${testSupportLibraryArguments} -o "${sha2SmokeOutput}"
    RESULT_VARIABLE sha2SmokeCompileResult
    ERROR_VARIABLE sha2SmokeCompileError
)
if(NOT sha2SmokeCompileResult EQUAL 0)
    message(FATAL_ERROR
        "SHA-2 smoke compilation failed: ${sha2SmokeCompileError}")
endif()
execute_process(
    COMMAND "${sha2SmokeOutput}"
    RESULT_VARIABLE sha2SmokeResult
)
if(NOT sha2SmokeResult EQUAL 42)
    message(FATAL_ERROR
        "SHA-2 smoke returned ${sha2SmokeResult} instead of 42")
endif()

# Exercise the installed standard library and runtime through a real TCP
# client/server exchange. The native helper supplies only the peer thread
# because Flecha does not have threading yet; both endpoints use the public
# runtime ABI and the server uses std::net::tcp.
set(tcpSmokeSource "${OUTPUT_ROOT}/tcp-smoke.fl")
file(WRITE "${tcpSmokeSource}" [=[
module tcp_smoke;

@imports
    :: lib::std::net::address
    :: lib::std::net::tcp
    :: lib::std::time::duration;

foreign c {
    #include <stddef.h>
    #include <stdint.h>

    #ifdef _WIN32
    #include <windows.h>
    static HANDLE tcp_smoke_thread;
    #else
    #include <pthread.h>
    static pthread_t tcp_smoke_thread;
    #endif

    extern int32_t flecha_runtime_v1_tcp_stream_connect(
        void**, uint8_t, uint64_t, uint64_t, uint16_t,
        uint8_t, uint64_t, uint32_t);
    extern int32_t flecha_runtime_v1_tcp_stream_read(
        void*, uint8_t*, size_t, size_t*);
    extern int32_t flecha_runtime_v1_tcp_stream_write(
        void*, const uint8_t*, size_t, size_t*);
    extern int32_t flecha_runtime_v1_tcp_stream_set_timeout(
        void*, uint8_t, uint8_t, uint64_t, uint32_t);
    extern int32_t flecha_runtime_v1_tcp_stream_close(void*);

    static int32_t tcp_smoke_status = 1;
    static int tcp_smoke_started = 0;

    static void tcp_smoke_run_client(uint16_t port) {
        void* stream = NULL;
        size_t count = 0;
        uint8_t request = 20;
        uint8_t response = 0;
        tcp_smoke_status = 2;
        if (flecha_runtime_v1_tcp_stream_connect(
                &stream, 4, 0, UINT64_C(0x7f000001), port, 1, 3, 0) != 0)
            return;
        if (flecha_runtime_v1_tcp_stream_set_timeout(
                stream, 0, 1, 3, 0) != 0 ||
            flecha_runtime_v1_tcp_stream_set_timeout(
                stream, 1, 1, 3, 0) != 0) {
            tcp_smoke_status = 3;
            flecha_runtime_v1_tcp_stream_close(stream);
            return;
        }
        if (flecha_runtime_v1_tcp_stream_write(
                stream, &request, 1, &count) != 0 || count != 1) {
            tcp_smoke_status = 4;
            flecha_runtime_v1_tcp_stream_close(stream);
            return;
        }
        count = 0;
        if (flecha_runtime_v1_tcp_stream_read(
                stream, &response, 1, &count) != 0 ||
            count != 1 || response != 22) {
            tcp_smoke_status = 5;
            flecha_runtime_v1_tcp_stream_close(stream);
            return;
        }
        tcp_smoke_status =
            flecha_runtime_v1_tcp_stream_close(stream) == 0 ? 0 : 6;
    }

    #ifdef _WIN32
    static DWORD WINAPI tcp_smoke_client(void* input) {
        tcp_smoke_run_client((uint16_t)(uintptr_t)input);
        return 0;
    }
    #else
    static void* tcp_smoke_client(void* input) {
        tcp_smoke_run_client((uint16_t)(uintptr_t)input);
        return NULL;
    }
    #endif

    int32_t tcp_smoke_start_client(uint16_t port) {
        if (tcp_smoke_started) return 7;
        tcp_smoke_started = 1;
        #ifdef _WIN32
        tcp_smoke_thread = CreateThread(
            NULL, 0, tcp_smoke_client, (void*)(uintptr_t)port, 0, NULL);
        if (tcp_smoke_thread == NULL) return 8;
        #else
        if (pthread_create(&tcp_smoke_thread, NULL, tcp_smoke_client,
                           (void*)(uintptr_t)port) != 0)
            return 8;
        #endif
        return 0;
    }

    int32_t tcp_smoke_join_client(void) {
        if (!tcp_smoke_started) return 9;
        #ifdef _WIN32
        if (WaitForSingleObject(tcp_smoke_thread, INFINITE) != WAIT_OBJECT_0)
            return 10;
        CloseHandle(tcp_smoke_thread);
        #else
        if (pthread_join(tcp_smoke_thread, NULL) != 0) return 10;
        #endif
        tcp_smoke_started = 0;
        return tcp_smoke_status;
    }
}

extern unsafe i32 tcp_smoke_start_client(u16 port);
extern unsafe i32 tcp_smoke_join_client();

i32 main() {
    endpoint := address::SocketAddress::fromIpv4(
        address::Ipv4SocketAddress::fromParts(
            address::Ipv4Address::loopback(), 0));
    listener, bindError := tcp::Listener::bind(endpoint, 8);
    if bindError != none { return 1; }
    local, addressError := listener.localAddress();
    localIpv4, isIpv4 := local.ipv4();
    if addressError != none or !isIpv4 or localIpv4.port() == 0 { return 2; }

    i32 startStatus = 0;
    unsafe { startStatus = tcp_smoke_start_client(localIpv4.port()); }
    if startStatus != 0 { return 3; }

    stream, _, acceptError := listener.accept();
    if acceptError != none { return 4; }
    if stream.setReadTimeout(duration::Duration::fromSeconds(3)) != none or
        stream.setWriteTimeout(duration::Duration::fromSeconds(3)) != none {
        return 5;
    }
    [1]u8 request = none;
    count, readError := stream.read(request[:]);
    if readError != none or count != 1 or request[0] != 20 { return 6; }
    [1]u8 response = [22];
    count, writeError := stream.write(response[:]);
    if writeError != none or count != 1 { return 7; }
    if stream.close() != none or listener.close() != none { return 8; }

    i32 clientStatus = 0;
    unsafe { clientStatus = tcp_smoke_join_client(); }
    if clientStatus != 0 { return 9; }
    return 42;
}
]=])

set(tcpSmokeOutput "${OUTPUT_ROOT}/tcp-smoke${EXECUTABLE_SUFFIX}")
execute_process(
    COMMAND "${FLECHA_COMPILER}" build "${tcpSmokeSource}"
        ${testSupportLibraryArguments} -o "${tcpSmokeOutput}"
    RESULT_VARIABLE tcpSmokeCompileResult
    ERROR_VARIABLE tcpSmokeCompileError
    TIMEOUT 60
)
if(NOT tcpSmokeCompileResult EQUAL 0)
    message(FATAL_ERROR
        "TCP client/server smoke compilation failed: ${tcpSmokeCompileError}")
endif()
execute_process(
    COMMAND "${tcpSmokeOutput}"
    RESULT_VARIABLE tcpSmokeResult
    ERROR_VARIABLE tcpSmokeError
    TIMEOUT 15
)
if(NOT tcpSmokeResult EQUAL 42)
    message(FATAL_ERROR
        "TCP client/server smoke returned ${tcpSmokeResult} instead of 42: "
        "${tcpSmokeError}")
endif()

set(callableRoot "${OUTPUT_ROOT}/callable-smoke")
file(MAKE_DIRECTORY "${callableRoot}")
file(WRITE "${callableRoot}/pipeline.fl"
    "module pipeline;\n"
    "export func Mapper(i32) => (i32);\n"
    "export func Setter() => ();\n"
    "export once func OnceAction() => (i32);\n"
    "export unsafe func Reader() => (i32);\n"
    "export structure Pipeline { Mapper callback; };\n"
    "export structure Counter { i32 current; };\n"
    "structure Token { i32 value; };\n"
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
    "export Setter assign(&i32 target, i32 value) <- target {\n"
    "    return [&target, copy value]() => () { *target = value; };\n"
    "}\n"
    "Counter Counter::init(i32 value) override {\n"
    "    return build Counter { self.current = value; };\n"
    "}\n"
    "void Counter::set(&self, i32 value) { self->current = value; }\n"
    "i32 Counter::get(&const self) { return self->current; }\n"
    "Token Token::init(i32 value) override {\n"
    "    return build Token { self.value = value; };\n"
    "}\n"
    "void Token::tear(&self) override {}\n"
    "i32 consume(Token token) { return token.value; }\n"
    "export OnceAction consume_once(i32 value) {\n"
    "    Token token = Token::init(value);\n"
    "    return once [own token]() => (i32) { return consume(token); };\n"
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
    "    pipeline::OnceAction onceAction = pipeline::consume_once(10);\n"
    "    i32 onceValue = onceAction();\n"
    "    i32 deferredValue = 0;\n"
    "    {\n"
    "        pipeline::Setter setter = pipeline::assign(&deferredValue, 20);\n"
    "        defer setter();\n"
    "    }\n"
    "    if deferredValue != 20 { return 8; }\n"
    "    pipeline::Counter counter = pipeline::Counter::init(0);\n"
    "    {\n"
    "        defer counter.set(12);\n"
    "    }\n"
    "    return onceValue + deferredValue + counter.get();\n"
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
    "@imports :: lib::std::containers::collections;\n"
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

if(DEFINED COMMUNITY_PACKAGE_ROOT)
    get_filename_component(
        COMMUNITY_PACKAGE_ROOT "${COMMUNITY_PACKAGE_ROOT}" ABSOLUTE)
    if(NOT EXISTS "${COMMUNITY_PACKAGE_ROOT}/longbow.toml" OR
       NOT EXISTS "${COMMUNITY_PACKAGE_ROOT}/longbow.lock" OR
       NOT EXISTS "${COMMUNITY_PACKAGE_ROOT}/src/main.fl")
        message(FATAL_ERROR
            "community package fixture is incomplete: "
            "${COMMUNITY_PACKAGE_ROOT}")
    endif()

    set(communityProject "${OUTPUT_ROOT}/community-package")
    set(communityCache "${OUTPUT_ROOT}/community-package-cache")
    file(MAKE_DIRECTORY "${communityProject}")
    file(COPY "${COMMUNITY_PACKAGE_ROOT}/" DESTINATION "${communityProject}")
    file(REMOVE_RECURSE "${communityProject}/target" "${communityCache}")
    file(SHA256 "${communityProject}/longbow.lock" communityLockBefore)

    # Start from an empty cache and download the immutable versions selected by
    # the checked lock. Both versions are intentionally yanked, proving that a
    # lock remains usable after new selection has been disabled.
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env
            "FLECHA_COMPILER=${FLECHA_COMPILER}"
            "LONGBOW_CACHE_DIR=${communityCache}"
            "${LONGBOW}" run
        WORKING_DIRECTORY "${communityProject}"
        RESULT_VARIABLE communityOnlineResult
        OUTPUT_VARIABLE communityOnlineOutput
        ERROR_VARIABLE communityOnlineError)
    if(NOT communityOnlineResult EQUAL 42)
        message(FATAL_ERROR
            "locked community package returned ${communityOnlineResult}, "
            "expected 42: ${communityOnlineOutput}${communityOnlineError}")
    endif()
    file(SHA256 "${communityProject}/longbow.lock" communityLockAfter)
    if(NOT communityLockBefore STREQUAL communityLockAfter)
        message(FATAL_ERROR
            "community package resolution changed the checked lock")
    endif()
    file(READ "${communityProject}/target/package-graph.json" communityGraph)
    foreach(required IN ITEMS
            "segfaultcafe/e2e-community-package"
            "segfaultcafe/e2e-community-leaf"
            "\"community\": \"registry-1\""
            "\"foundation\": \"registry-2\"")
        if(NOT communityGraph MATCHES "${required}")
            message(FATAL_ERROR
                "installed package graph is missing ${required}: "
                "${communityGraph}")
        endif()
    endforeach()

    # Force registry requests through an unreachable loopback proxy. Longbow
    # must fall back to the verified cached indexes and archives and rebuild
    # after its target directory is removed.
    file(REMOVE_RECURSE "${communityProject}/target")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env
            "FLECHA_COMPILER=${FLECHA_COMPILER}"
            "LONGBOW_CACHE_DIR=${communityCache}"
            "HTTPS_PROXY=http://127.0.0.1:1"
            "HTTP_PROXY=http://127.0.0.1:1"
            "ALL_PROXY=http://127.0.0.1:1"
            "NO_PROXY="
            "https_proxy=http://127.0.0.1:1"
            "http_proxy=http://127.0.0.1:1"
            "all_proxy=http://127.0.0.1:1"
            "no_proxy="
            "${LONGBOW}" run
        WORKING_DIRECTORY "${communityProject}"
        RESULT_VARIABLE communityOfflineResult
        OUTPUT_VARIABLE communityOfflineOutput
        ERROR_VARIABLE communityOfflineError)
    if(NOT communityOfflineResult EQUAL 42)
        message(FATAL_ERROR
            "offline community package returned ${communityOfflineResult}, "
            "expected 42: ${communityOfflineOutput}${communityOfflineError}")
    endif()
endif()

if(DEFINED STANDARD_LIBRARY_EXAMPLE_ROOT)
    get_filename_component(STANDARD_LIBRARY_EXAMPLE_ROOT
        "${STANDARD_LIBRARY_EXAMPLE_ROOT}" ABSOLUTE)
    foreach(source IN ITEMS main.fl public_api.fl public_api_test.fl)
        if(NOT EXISTS "${STANDARD_LIBRARY_EXAMPLE_ROOT}/src/${source}")
            message(FATAL_ERROR
                "standard-library example is missing src/${source}: "
                "${STANDARD_LIBRARY_EXAMPLE_ROOT}")
        endif()
    endforeach()

    set(standardLibraryProject "${OUTPUT_ROOT}/standard-library")
    file(MAKE_DIRECTORY "${standardLibraryProject}")
    file(COPY "${STANDARD_LIBRARY_EXAMPLE_ROOT}/src"
        DESTINATION "${standardLibraryProject}")
    file(WRITE "${standardLibraryProject}/longbow.toml" [=[
[package]
name = "standard_library"
version = "0.1.0"
kind = "program"

[dependencies]
]=])

    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env
            "FLECHA_COMPILER=${FLECHA_COMPILER}"
            "${LONGBOW}" test
        WORKING_DIRECTORY "${standardLibraryProject}"
        RESULT_VARIABLE standardLibraryTestResult
        OUTPUT_VARIABLE standardLibraryTestOutput
        ERROR_VARIABLE standardLibraryTestError)
    if(NOT standardLibraryTestResult EQUAL 0)
        message(FATAL_ERROR
            "installed standard-library example tests failed: "
            "${standardLibraryTestOutput}${standardLibraryTestError}")
    endif()

    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env
            "FLECHA_COMPILER=${FLECHA_COMPILER}"
            "${LONGBOW}" build
        WORKING_DIRECTORY "${standardLibraryProject}"
        RESULT_VARIABLE standardLibraryBuildResult
        OUTPUT_VARIABLE standardLibraryBuildOutput
        ERROR_VARIABLE standardLibraryBuildError)
    if(NOT standardLibraryBuildResult EQUAL 0)
        message(FATAL_ERROR
            "installed standard-library example build failed: "
            "${standardLibraryBuildOutput}${standardLibraryBuildError}")
    endif()

    set(standardLibraryExecutable
        "${standardLibraryProject}/target/standard_library${EXECUTABLE_SUFFIX}")
    if(NOT EXISTS "${standardLibraryExecutable}")
        message(FATAL_ERROR
            "installed standard-library example executable was not produced")
    endif()
    execute_process(
        COMMAND "${standardLibraryExecutable}"
        RESULT_VARIABLE standardLibraryRunResult
        OUTPUT_VARIABLE standardLibraryRunOutput
        ERROR_VARIABLE standardLibraryRunError)
    string(REPLACE "\r\n" "\n"
        standardLibraryRunOutput "${standardLibraryRunOutput}")
    if(NOT standardLibraryRunResult EQUAL 0 OR
       NOT standardLibraryRunOutput STREQUAL
           "standard library API tour: ok\n")
        message(FATAL_ERROR
            "installed standard-library example run failed: "
            "${standardLibraryRunOutput}${standardLibraryRunError}")
    endif()
endif()

if(DEFINED WORKSPACE_GUARD_ROOT)
    get_filename_component(WORKSPACE_GUARD_ROOT
        "${WORKSPACE_GUARD_ROOT}" ABSOLUTE)
    foreach(source IN ITEMS main.fl model.fl scanner.fl manifest.fl commands.fl)
        if(NOT EXISTS "${WORKSPACE_GUARD_ROOT}/src/${source}")
            message(FATAL_ERROR
                "workspace_guard fixture is missing src/${source}: "
                "${WORKSPACE_GUARD_ROOT}")
        endif()
    endforeach()

    set(workspaceGuardProject "${OUTPUT_ROOT}/workspace-guard")
    set(workspaceGuardTree "${OUTPUT_ROOT}/workspace-guard-tree")
    set(workspaceGuardRelocated "${OUTPUT_ROOT}/workspace-guard-relocated")
    file(MAKE_DIRECTORY
        "${workspaceGuardProject}"
        "${workspaceGuardTree}/nested"
        "${workspaceGuardRelocated}/nested")
    file(COPY "${WORKSPACE_GUARD_ROOT}/src"
        DESTINATION "${workspaceGuardProject}")
    file(WRITE "${workspaceGuardProject}/longbow.toml" [=[
[package]
name = "workspace_guard"
version = "0.1.0"
kind = "program"

[dependencies]
]=])

    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env
            "FLECHA_COMPILER=${FLECHA_COMPILER}"
            "${LONGBOW}" build
        WORKING_DIRECTORY "${workspaceGuardProject}"
        RESULT_VARIABLE workspaceGuardBuildResult
        OUTPUT_VARIABLE workspaceGuardBuildOutput
        ERROR_VARIABLE workspaceGuardBuildError)
    if(NOT workspaceGuardBuildResult EQUAL 0)
        message(FATAL_ERROR
            "installed workspace_guard build failed: "
            "${workspaceGuardBuildOutput}${workspaceGuardBuildError}")
    endif()

    set(workspaceGuardExecutable
        "${workspaceGuardProject}/target/workspace_guard${EXECUTABLE_SUFFIX}")
    if(NOT EXISTS "${workspaceGuardExecutable}")
        message(FATAL_ERROR
            "installed workspace_guard executable was not produced")
    endif()

    file(WRITE "${workspaceGuardTree}/alpha.txt" "alpha\n")
    file(WRITE "${workspaceGuardTree}/nested/beta.txt" "beta\n")
    file(WRITE "${workspaceGuardTree}/space name.txt" "space\n")
    file(COPY "${workspaceGuardTree}/alpha.txt"
        DESTINATION "${workspaceGuardRelocated}")
    file(COPY "${workspaceGuardTree}/nested/beta.txt"
        DESTINATION "${workspaceGuardRelocated}/nested")
    file(COPY "${workspaceGuardTree}/space name.txt"
        DESTINATION "${workspaceGuardRelocated}")

    set(workspaceGuardManifest "${OUTPUT_ROOT}/workspace-guard.manifest")
    set(workspaceGuardRelocatedManifest
        "${OUTPUT_ROOT}/workspace-guard-relocated.manifest")
    execute_process(
        COMMAND "${workspaceGuardExecutable}" snapshot
            "${workspaceGuardTree}" "${workspaceGuardManifest}"
        RESULT_VARIABLE workspaceGuardSnapshotResult
        OUTPUT_VARIABLE workspaceGuardSnapshotOutput
        ERROR_VARIABLE workspaceGuardSnapshotError)
    string(REPLACE "\r\n" "\n"
        workspaceGuardSnapshotOutput "${workspaceGuardSnapshotOutput}")
    if(NOT workspaceGuardSnapshotResult EQUAL 0 OR
       NOT workspaceGuardSnapshotOutput STREQUAL "snapshot: 3 files\n")
        message(FATAL_ERROR
            "installed workspace_guard snapshot failed: "
            "${workspaceGuardSnapshotOutput}${workspaceGuardSnapshotError}")
    endif()

    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env
            "FLECHA_GUARD_ROOT=${workspaceGuardTree}"
            "${workspaceGuardExecutable}" verify "${workspaceGuardManifest}"
        RESULT_VARIABLE workspaceGuardVerifyResult
        OUTPUT_VARIABLE workspaceGuardVerifyOutput
        ERROR_VARIABLE workspaceGuardVerifyError)
    string(REPLACE "\r\n" "\n"
        workspaceGuardVerifyOutput "${workspaceGuardVerifyOutput}")
    if(NOT workspaceGuardVerifyResult EQUAL 0 OR
       NOT workspaceGuardVerifyOutput STREQUAL "verified: 3 files\n")
        message(FATAL_ERROR
            "installed workspace_guard verification failed: "
            "${workspaceGuardVerifyOutput}${workspaceGuardVerifyError}")
    endif()

    execute_process(
        COMMAND "${workspaceGuardExecutable}" snapshot
            "${workspaceGuardRelocated}" "${workspaceGuardRelocatedManifest}"
        RESULT_VARIABLE workspaceGuardRelocatedResult
        OUTPUT_VARIABLE workspaceGuardRelocatedOutput
        ERROR_VARIABLE workspaceGuardRelocatedError)
    if(NOT workspaceGuardRelocatedResult EQUAL 0)
        message(FATAL_ERROR
            "relocated workspace_guard snapshot failed: "
            "${workspaceGuardRelocatedOutput}${workspaceGuardRelocatedError}")
    endif()
    file(SHA256 "${workspaceGuardManifest}" workspaceGuardHash)
    file(SHA256 "${workspaceGuardRelocatedManifest}"
        workspaceGuardRelocatedHash)
    if(NOT workspaceGuardHash STREQUAL workspaceGuardRelocatedHash)
        message(FATAL_ERROR
            "relocated workspace_guard tree changed its canonical manifest")
    endif()

    execute_process(
        COMMAND "${workspaceGuardExecutable}" run
            "${workspaceGuardTree}" "${workspaceGuardManifest}" --
            "${CMAKE_COMMAND}" -E true
        RESULT_VARIABLE workspaceGuardRunResult
        OUTPUT_VARIABLE workspaceGuardRunOutput
        ERROR_VARIABLE workspaceGuardRunError)
    if(NOT workspaceGuardRunResult EQUAL 0)
        message(FATAL_ERROR
            "installed workspace_guard child execution failed: "
            "${workspaceGuardRunOutput}${workspaceGuardRunError}")
    endif()

    file(WRITE "${workspaceGuardTree}/nested/beta.txt" "changed\n")
    execute_process(
        COMMAND "${workspaceGuardExecutable}" verify
            "${workspaceGuardTree}" "${workspaceGuardManifest}"
        RESULT_VARIABLE workspaceGuardMismatchResult
        OUTPUT_VARIABLE workspaceGuardMismatchOutput
        ERROR_VARIABLE workspaceGuardMismatchError)
    string(REPLACE "\r\n" "\n"
        workspaceGuardMismatchError "${workspaceGuardMismatchError}")
    if(NOT workspaceGuardMismatchResult EQUAL 5 OR
       NOT workspaceGuardMismatchError STREQUAL
           "modified nested/beta.txt\nworkspace_guard: 1 workspace differences\n")
        message(FATAL_ERROR
            "installed workspace_guard mismatch contract failed: "
            "${workspaceGuardMismatchOutput}${workspaceGuardMismatchError}")
    endif()
endif()

if(DEFINED LEDGER_ROOT)
    get_filename_component(LEDGER_ROOT "${LEDGER_ROOT}" ABSOLUTE)
    foreach(source IN ITEMS
        main.fl model.fl codec.fl storage.fl commands.fl codec_test.fl)
        if(NOT EXISTS "${LEDGER_ROOT}/src/${source}")
            message(FATAL_ERROR
                "ledger fixture is missing src/${source}: ${LEDGER_ROOT}")
        endif()
    endforeach()

    set(ledgerProject "${OUTPUT_ROOT}/ledger")
    file(MAKE_DIRECTORY "${ledgerProject}")
    file(COPY "${LEDGER_ROOT}/src" DESTINATION "${ledgerProject}")
    file(WRITE "${ledgerProject}/longbow.toml" [=[
[package]
name = "ledger"
version = "0.1.0"
kind = "program"

[dependencies]
]=])

    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env
            "FLECHA_COMPILER=${FLECHA_COMPILER}"
            "${LONGBOW}" test
        WORKING_DIRECTORY "${ledgerProject}"
        RESULT_VARIABLE ledgerTestResult
        OUTPUT_VARIABLE ledgerTestOutput
        ERROR_VARIABLE ledgerTestError)
    if(NOT ledgerTestResult EQUAL 0)
        message(FATAL_ERROR
            "installed ledger golden tests failed: "
            "${ledgerTestOutput}${ledgerTestError}")
    endif()

    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env
            "FLECHA_COMPILER=${FLECHA_COMPILER}"
            "${LONGBOW}" build
        WORKING_DIRECTORY "${ledgerProject}"
        RESULT_VARIABLE ledgerBuildResult
        OUTPUT_VARIABLE ledgerBuildOutput
        ERROR_VARIABLE ledgerBuildError)
    if(NOT ledgerBuildResult EQUAL 0)
        message(FATAL_ERROR
            "installed ledger build failed: "
            "${ledgerBuildOutput}${ledgerBuildError}")
    endif()

    set(ledgerExecutable
        "${ledgerProject}/target/ledger${EXECUTABLE_SUFFIX}")
    if(NOT EXISTS "${ledgerExecutable}")
        message(FATAL_ERROR "installed ledger executable was not produced")
    endif()

    set(ledgerFirst "${OUTPUT_ROOT}/ledger-first.fld")
    set(ledgerSecond "${OUTPUT_ROOT}/ledger-second.fld")
    foreach(ledgerPath IN ITEMS "${ledgerFirst}" "${ledgerSecond}")
        execute_process(
            COMMAND "${ledgerExecutable}" create "${ledgerPath}"
            RESULT_VARIABLE ledgerCreateResult
            OUTPUT_VARIABLE ledgerCreateOutput
            ERROR_VARIABLE ledgerCreateError)
        if(NOT ledgerCreateResult EQUAL 0)
            message(FATAL_ERROR
                "installed ledger create failed: "
                "${ledgerCreateOutput}${ledgerCreateError}")
        endif()
        execute_process(
            COMMAND "${ledgerExecutable}" append "${ledgerPath}" alpha
            RESULT_VARIABLE ledgerAlphaResult
            OUTPUT_VARIABLE ledgerAlphaOutput
            ERROR_VARIABLE ledgerAlphaError)
        if(NOT ledgerAlphaResult EQUAL 0)
            message(FATAL_ERROR
                "installed ledger genesis append failed: "
                "${ledgerAlphaOutput}${ledgerAlphaError}")
        endif()
        execute_process(
            COMMAND "${ledgerExecutable}" append "${ledgerPath}" beta
            RESULT_VARIABLE ledgerBetaResult
            OUTPUT_VARIABLE ledgerBetaOutput
            ERROR_VARIABLE ledgerBetaError)
        if(NOT ledgerBetaResult EQUAL 0)
            message(FATAL_ERROR
                "installed ledger second append failed: "
                "${ledgerBetaOutput}${ledgerBetaError}")
        endif()
    endforeach()

    execute_process(
        COMMAND "${ledgerExecutable}" verify "${ledgerFirst}"
        RESULT_VARIABLE ledgerVerifyResult
        OUTPUT_VARIABLE ledgerVerifyOutput
        ERROR_VARIABLE ledgerVerifyError)
    string(REPLACE "\r\n" "\n" ledgerVerifyOutput "${ledgerVerifyOutput}")
    if(NOT ledgerVerifyResult EQUAL 0 OR
       NOT ledgerVerifyOutput STREQUAL
           "verified: 2 blocks d4298ba679357d6f92c5ea1da21f8895a3e6d514b35e51b3e68081744d9f6b4a\n")
        message(FATAL_ERROR
            "installed ledger verification failed: "
            "${ledgerVerifyOutput}${ledgerVerifyError}")
    endif()

    file(SHA256 "${ledgerFirst}" ledgerFirstHash)
    file(SHA256 "${ledgerSecond}" ledgerSecondHash)
    if(NOT ledgerFirstHash STREQUAL
       "8597adac0ae2a0efee59c082eb1ea4e68fa5752d2ab30bfdfc7d5b6743a95847" OR
       NOT ledgerFirstHash STREQUAL ledgerSecondHash)
        message(FATAL_ERROR
            "installed ledger did not reproduce its canonical bytes")
    endif()

    file(WRITE "${OUTPUT_ROOT}/ledger-malformed.fld" "not-a-ledger")
    execute_process(
        COMMAND "${ledgerExecutable}" verify
            "${OUTPUT_ROOT}/ledger-malformed.fld"
        RESULT_VARIABLE ledgerMalformedResult
        OUTPUT_VARIABLE ledgerMalformedOutput
        ERROR_VARIABLE ledgerMalformedError)
    string(REPLACE "\r\n" "\n"
        ledgerMalformedError "${ledgerMalformedError}")
    if(NOT ledgerMalformedResult EQUAL 4 OR
       NOT ledgerMalformedError STREQUAL "ledger: invalid ledger format\n")
        message(FATAL_ERROR
            "installed ledger malformed-input contract failed: "
            "${ledgerMalformedOutput}${ledgerMalformedError}")
    endif()
endif()
