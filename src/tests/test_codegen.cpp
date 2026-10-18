#include <gtest/gtest.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/Program.h>
#include <llvm/Support/raw_ostream.h>

#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "codegen/llvm_code_generator.hpp"
#include "codegen/native_compiler.hpp"
#include "core/parser.hpp"
#include "core/tokenizer.hpp"
#include "lowering/lowerer.hpp"
#include "semantic/semantic_analyzer.hpp"

using namespace flecha::codegen;
using namespace flecha::core;
using namespace flecha::diagnostics;
using namespace flecha::lowering;
using namespace flecha::semantic;

namespace {

void SetEnvironment(const std::string& name,
                    const std::optional<std::string>& value) {
#ifdef _WIN32
    _putenv_s(name.c_str(), value.value_or("").c_str());
#else
    if (value.has_value())
        setenv(name.c_str(), value->c_str(), 1);
    else
        unsetenv(name.c_str());
#endif
}

class ScopedEnvironment final {
   public:
    ScopedEnvironment(std::string name, std::optional<std::string> value)
        : _name(std::move(name)) {
        if (const char* previous = std::getenv(_name.c_str()))
            _previous = previous;
        SetEnvironment(_name, value);
    }

    ~ScopedEnvironment() { SetEnvironment(_name, _previous); }

    ScopedEnvironment(const ScopedEnvironment&) = delete;
    ScopedEnvironment& operator=(const ScopedEnvironment&) = delete;

   private:
    std::string _name;
    std::optional<std::string> _previous;
};

struct GeneratedSource final {
    DiagnosticBag diagnostics;
    std::unique_ptr<CompilationUnit> unit;
    SemanticModel semanticModel;
    std::unique_ptr<BoundProgram> boundProgram;
    std::unique_ptr<LlvmCodeGenerator> generator;
    std::unique_ptr<llvm::Module> module;
};

GeneratedSource Generate(const std::string& source) {
    GeneratedSource result;
    Tokenizer tokenizer(source, result.diagnostics);
    std::vector<Token> tokens = tokenizer.Tokenize();
    Parser parser(std::move(tokens), result.diagnostics);
    result.unit = parser.Parse();
    SemanticAnalyzer analyzer(result.diagnostics);
    result.semanticModel = analyzer.Analyze(*result.unit);
    Lowerer lowerer;
    result.boundProgram =
        lowerer.Lower(*result.unit, result.semanticModel, result.diagnostics);
    if (result.boundProgram != nullptr) {
        result.generator = std::make_unique<LlvmCodeGenerator>();
        result.module =
            result.generator->Generate(*result.boundProgram, "test_module");
    }
    return result;
}

std::string ModuleText(const llvm::Module& module) {
    std::string text;
    llvm::raw_string_ostream output(text);
    module.print(output, nullptr);
    output.flush();
    return text;
}

}  // namespace

TEST(LlvmCodeGeneratorTests, EmitsVerifiedFunctionsLocalsCallsAndReturns) {
    GeneratedSource result = Generate(
        "i32 add(i32 left, i32 right) { return left + right; } "
        "i32 main() { x := 20; y := 22; return add(x, y); }");

    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.module, nullptr) << result.generator->Error();
    EXPECT_TRUE(VerifyModule(*result.module).empty());
    const std::string ir = ModuleText(*result.module);
    EXPECT_NE(ir.find("define i32 @add(i32 %left, i32 %right)"),
              std::string::npos);
    EXPECT_NE(ir.find("%x = alloca i32"), std::string::npos);
    EXPECT_NE(ir.find("call i32 @add"), std::string::npos);
}

TEST(LlvmCodeGeneratorTests, EmitsForwardAndRecursiveCalls) {
    GeneratedSource result = Generate(
        "i32 main() { return Add(Factorial(3), 36); } "
        "i32 Add(i32 left, i32 right) { return left + right; } "
        "i32 Factorial(i32 value) { if value <= 1 { return 1; } "
        "return value * Factorial(value - 1); }");

    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.module, nullptr) << result.generator->Error();
    EXPECT_TRUE(VerifyModule(*result.module).empty());
    const std::string ir = ModuleText(*result.module);
    EXPECT_NE(ir.find("call i32 @Add"), std::string::npos);
    EXPECT_NE(ir.find("call i32 @Factorial"), std::string::npos);
}

TEST(LlvmCodeGeneratorTests, EmitsEnumDiscriminantsDefaultsAndMethods) {
    GeneratedSource result = Generate(
        "enum Status: u8 { default Idle = 3, Running = 7, Done, } "
        "bool Status::is_running(&const self) { "
        "return *self == Status.Running; } "
        "Status Status::done() { return Status.Done; } "
        "i32 main() { Status status = none; [2]Status values = none; "
        "if status != none { return 2; } status = Status.Running; "
        "if status.is_running() and "
        "values[0] == Status.Idle and Status::done() == Status.Done { "
        "return 0; } return 1; }");

    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.module, nullptr) << result.generator->Error();
    EXPECT_TRUE(VerifyModule(*result.module).empty());
    const std::string ir = ModuleText(*result.module);
    EXPECT_NE(ir.find("define i1 @\"Status::is_running\"(ptr"),
              std::string::npos);
    EXPECT_NE(ir.find("define i8 @\"Status::done\"()"), std::string::npos);
    EXPECT_NE(ir.find("store i8 3"), std::string::npos);
    EXPECT_NE(ir.find("store i8 7"), std::string::npos);
    EXPECT_NE(ir.find("ret i8 8"), std::string::npos);
}

TEST(LlvmCodeGeneratorTests, EmitsVerifiedEnumMatchSwitches) {
    GeneratedSource result = Generate(R"(
        enum TokenKind: u8 { Identifier = 2, Number = 7, End = 9, }
        i32 classify(TokenKind kind) {
            return match kind {
                TokenKind.Identifier => 10,
                TokenKind.Number => 20,
                TokenKind.End => 42,
            };
        }
        i32 main() { return classify(TokenKind.End); }
    )");

    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.module, nullptr) << result.generator->Error();
    EXPECT_TRUE(VerifyModule(*result.module).empty());
    const std::string ir = ModuleText(*result.module);
    EXPECT_NE(ir.find("switch i8"), std::string::npos);
    EXPECT_NE(ir.find("i8 2, label %match.arm"), std::string::npos);
    EXPECT_NE(ir.find("i8 7, label %match.arm"), std::string::npos);
    EXPECT_NE(ir.find("i8 9, label %match.arm"), std::string::npos);
    EXPECT_NE(ir.find("match.invalid"), std::string::npos);
}

TEST(LlvmCodeGeneratorTests, EvaluatesMatchInputsOnceAndUsesWildcardDefault) {
    GeneratedSource result = Generate(R"(
        enum TokenKind: u8 { Identifier, End, }
        TokenKind next() { return TokenKind.End; }
        i32 main() {
            return match next() {
                TokenKind.Identifier => 1,
                _ => 42,
            };
        }
    )");

    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.module, nullptr) << result.generator->Error();
    EXPECT_TRUE(VerifyModule(*result.module).empty());
    const std::string ir = ModuleText(*result.module);
    const std::size_t call = ir.find("call i8 @next()");
    ASSERT_NE(call, std::string::npos);
    EXPECT_EQ(ir.find("call i8 @next()", call + 1), std::string::npos);
    EXPECT_EQ(ir.find("match.invalid"), std::string::npos);
}

TEST(LlvmCodeGeneratorTests, DefaultConstructsRuntimeEnumArrays) {
    GeneratedSource result = Generate(
        "enum Status: u8 { default Idle = 3, Running = 7, } "
        "unsafe i32 main() { values, failure := new<[]Status>(2); "
        "if failure != none { return 1; } i32 answer = 0; "
        "if values[0] != Status.Idle or values[1] != Status.Idle { "
        "answer = 2; } free(values); return answer; }");

    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.module, nullptr) << result.generator->Error();
    EXPECT_TRUE(VerifyModule(*result.module).empty());
    const std::string ir = ModuleText(*result.module);
    EXPECT_NE(ir.find("allocation.zero.condition"), std::string::npos);
    EXPECT_NE(ir.find("store i8 3"), std::string::npos);
}

TEST(LlvmCodeGeneratorTests, CarriesRuntimeArrayLengthInTheOwningValue) {
    GeneratedSource result = Generate(
        "unsafe i32 main() { values, failure := new<[]i32>(4); "
        "if failure != none { return 1; } owner := values; defer free(owner); "
        "owner[3] = 42; return owner[3]; }");

    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.module, nullptr) << result.generator->Error();
    EXPECT_TRUE(VerifyModule(*result.module).empty());
    const std::string ir = ModuleText(*result.module);
    EXPECT_NE(ir.find("allocation.owner"), std::string::npos);
    EXPECT_NE(ir.find("moved.value"), std::string::npos);
    EXPECT_NE(ir.find("free.data"), std::string::npos);
    EXPECT_EQ(ir.find("flecha_runtime_v1_allocation_count"), std::string::npos);
}

TEST(LlvmCodeGeneratorTests, TransfersFreshStringConcatenationResults) {
    GeneratedSource result =
        Generate("void f() { string text = \"\"; text = text + \"x\"; }");

    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.module, nullptr) << result.generator->Error();
    EXPECT_TRUE(VerifyModule(*result.module).empty());
    const std::string ir = ModuleText(*result.module);
    EXPECT_NE(ir.find("flecha_runtime_v1_string_concat"), std::string::npos);
    EXPECT_EQ(ir.find("flecha_runtime_v1_string_clone"), std::string::npos);
}

TEST(LlvmCodeGeneratorTests, EmitsExternCallsWithoutFunctionBodies) {
    GeneratedSource result = Generate(
        "extern unsafe i32 native_add(i32 left, i32 right); "
        "i32 add(i32 left, i32 right) { unsafe { return "
        "native_add(left, right); } }");

    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.module, nullptr) << result.generator->Error();
    const std::string ir = ModuleText(*result.module);
    EXPECT_NE(ir.find("declare i32 @native_add(i32, i32)"), std::string::npos);
    EXPECT_EQ(ir.find("define i32 @native_add"), std::string::npos);
    EXPECT_NE(ir.find("call i32 @native_add"), std::string::npos);
}

TEST(LlvmCodeGeneratorTests, UsesTargetCAbiIntegerExtensionAttributes) {
    GeneratedSource result = Generate(
        "extern i8 native_i8(i8 value); "
        "extern u16 native_u16(u16 value); "
        "i32 use(i8 signed_value, u16 unsigned_value) { "
        "native_i8(signed_value); native_u16(unsigned_value); return 0; }");
    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.boundProgram, nullptr);

    LlvmCodeGenerator x86Generator;
    x86Generator.ConfigureTarget("x86_64-unknown-linux-gnu",
                                 llvm::DataLayout("e-p:64:64"));
    std::unique_ptr<llvm::Module> x86Module =
        x86Generator.Generate(*result.boundProgram, "extern_x86_abi");
    ASSERT_NE(x86Module, nullptr) << x86Generator.Error();
    const std::string x86Ir = ModuleText(*x86Module);
    EXPECT_NE(x86Ir.find("declare signext i8 @native_i8(i8 signext)"),
              std::string::npos);
    EXPECT_NE(x86Ir.find("declare zeroext i16 @native_u16(i16 zeroext)"),
              std::string::npos);
    EXPECT_NE(x86Ir.find("call signext i8 @native_i8(i8 signext"),
              std::string::npos);
    EXPECT_NE(x86Ir.find("call zeroext i16 @native_u16(i16 zeroext"),
              std::string::npos);

    LlvmCodeGenerator aarch64Generator;
    aarch64Generator.ConfigureTarget("aarch64-unknown-linux-gnu",
                                     llvm::DataLayout("e-p:64:64"));
    std::unique_ptr<llvm::Module> aarch64Module =
        aarch64Generator.Generate(*result.boundProgram, "extern_aarch64_abi");
    ASSERT_NE(aarch64Module, nullptr) << aarch64Generator.Error();
    const std::string aarch64Ir = ModuleText(*aarch64Module);
    EXPECT_NE(aarch64Ir.find("declare i8 @native_i8(i8)"), std::string::npos);
    EXPECT_NE(aarch64Ir.find("declare i16 @native_u16(i16)"),
              std::string::npos);
}

TEST(LlvmCodeGeneratorTests, EmitsAssignmentsAndConditionalControlFlow) {
    GeneratedSource result = Generate(
        "i32 main() { value := 1; if value < 2 { value = value + 41; } "
        "else { value = 0; } return value; }");

    ASSERT_NE(result.module, nullptr) << result.generator->Error();
    const std::string ir = ModuleText(*result.module);
    EXPECT_NE(ir.find("br i1"), std::string::npos);
    EXPECT_NE(ir.find("if.then"), std::string::npos);
    EXPECT_NE(ir.find("if.else"), std::string::npos);
    EXPECT_NE(ir.find("store i32"), std::string::npos);
}

TEST(LlvmCodeGeneratorTests, EmitsWhileAndConditionalAlsoControlFlow) {
    GeneratedSource result = Generate(
        "i32 main() { value := 0; counter := 0; while counter < 4 { "
        "value = value + 10; counter = counter + 1; } "
        "also counter % 2 == 0 { value = value + 1; } return value; }");

    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.module, nullptr) << result.generator->Error();
    const std::string ir = ModuleText(*result.module);
    EXPECT_NE(ir.find("while.condition"), std::string::npos);
    EXPECT_NE(ir.find("while.body"), std::string::npos);
    EXPECT_NE(ir.find("while.end"), std::string::npos);
    EXPECT_NE(ir.find("if.then"), std::string::npos);
}

TEST(LlvmCodeGeneratorTests, EmitsLoopControlAndCrossNestedJumps) {
    GeneratedSource result = Generate(
        "i32 main() { value := 0; warmup := 0; while warmup < 1 { "
        "warmup = warmup + 1; proceed; } while true { if value == 0 { "
        "jumpto inner_position; } #outer_position break; while true { "
        "#inner_position value = 42; jumpto outer_position; } } "
        "return value; }");

    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.module, nullptr) << result.generator->Error();
    EXPECT_TRUE(VerifyModule(*result.module).empty());
    const std::string ir = ModuleText(*result.module);
    EXPECT_NE(ir.find("position.outer_position"), std::string::npos);
    EXPECT_NE(ir.find("position.inner_position"), std::string::npos);
}

TEST(LlvmCodeGeneratorTests, EmitsChainedNumericConversions) {
    GeneratedSource result = Generate(
        "i32 main() { base := 42; wide := base.cast<i64>; "
        "narrow := wide.cast<u8>; "
        "decimal := narrow.cast<f64>; result := (decimal as f32) as i32; "
        "return result; }");

    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.module, nullptr) << result.generator->Error();
    EXPECT_TRUE(VerifyModule(*result.module).empty());
    const std::string ir = ModuleText(*result.module);
    EXPECT_NE(ir.find("sext i32"), std::string::npos);
    EXPECT_NE(ir.find("trunc i64"), std::string::npos);
    EXPECT_NE(ir.find("uitofp i8"), std::string::npos);
    EXPECT_NE(ir.find("fptrunc double"), std::string::npos);
    EXPECT_NE(ir.find("fptosi float"), std::string::npos);
}

TEST(LlvmCodeGeneratorTests, EmitsShortCircuitBooleanLogic) {
    GeneratedSource result =
        Generate("bool both(bool left, bool right) { return left and right; }");

    ASSERT_NE(result.module, nullptr) << result.generator->Error();
    const std::string ir = ModuleText(*result.module);
    EXPECT_NE(ir.find("logic.right"), std::string::npos);
    EXPECT_NE(ir.find("phi i1"), std::string::npos);
}

TEST(LlvmCodeGeneratorTests, EmitsStringViewsAndContentEquality) {
    GeneratedSource result = Generate(
        "stringv Identity(stringv value) <- value { return value; } "
        "bool Equal(stringv left, stringv right) { return left == right; } "
        "i32 main() { value := Identity(\"Flecha\"); "
        "if Equal(value, \"Flecha\") and value != \"Arrow\" { return 42; } "
        "return 0; }");

    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.module, nullptr) << result.generator->Error();
    EXPECT_TRUE(VerifyModule(*result.module).empty());
    const std::string ir = ModuleText(*result.module);
    EXPECT_NE(ir.find("@memcmp"), std::string::npos);
    EXPECT_NE(ir.find("stringv.content"), std::string::npos);
    EXPECT_NE(ir.find("{ ptr, i64 }"), std::string::npos);
}

TEST(LlvmCodeGeneratorTests, EmitsCheckedStringViewOperations) {
    GeneratedSource result = Generate(
        "i32 main() { text := \"flecha\"; first := text.at(0); "
        "last := text[5]; part := text.substr(1, 3); length := text.len(); "
        "return 0; }");

    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.module, nullptr) << result.generator->Error();
    EXPECT_TRUE(VerifyModule(*result.module).empty());
    const std::string ir = ModuleText(*result.module);
    EXPECT_NE(ir.find("@flecha_runtime_v1_string_view_at"), std::string::npos);
    EXPECT_NE(ir.find("@flecha_runtime_v1_string_view_check_substr"),
              std::string::npos);
    EXPECT_NE(ir.find("getelementptr inbounds i8"), std::string::npos);
    EXPECT_NE(ir.find("stringv.len"), std::string::npos);
}

TEST(LlvmCodeGeneratorTests, EmitsOwnedStringConversionsAndCleanup) {
    GeneratedSource result = Generate(
        "i32 main() { string greeting = \"hi mom\".string(); "
        "string copy = greeting; string answer = 42.string(); "
        "stringv view = greeting.view(); "
        "stdoutln(greeting); return 42; }");

    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.module, nullptr) << result.generator->Error();
    EXPECT_TRUE(VerifyModule(*result.module).empty());
    const std::string ir = ModuleText(*result.module);
    EXPECT_NE(ir.find("@flecha_runtime_v1_string_from_view"),
              std::string::npos);
    EXPECT_NE(ir.find("@flecha_runtime_v1_string_from_i32"), std::string::npos);
    EXPECT_NE(ir.find("@flecha_runtime_v1_string_destroy"), std::string::npos);
    EXPECT_NE(ir.find("@flecha_runtime_v1_stdout_string_view"),
              std::string::npos);
}

TEST(LlvmCodeGeneratorTests, EmitsLengthAwareStandardOutput) {
    GeneratedSource result = Generate(
        "i32 main() { stdout(); stdout(\"Flecha\", -42, true); "
        "stdoutln(); stdoutln(\"Language\"); stdout(1 as i8); "
        "stdout(2 as i16); stdout(3 as i64); stdout(4 as isize); "
        "stdout(5 as u8); stdout(6 as u16); stdout(7 as u32); "
        "stdout(8 as u64); stdout(9 as usize); stdout(1 as f32); "
        "stdout(1.5); stdout(true); stdout('x'); return 42; }");

    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.module, nullptr) << result.generator->Error();
    EXPECT_TRUE(VerifyModule(*result.module).empty());
    const std::string ir = ModuleText(*result.module);
    const std::string suffixes[] = {"string_view", "i8",    "i16",   "i32",
                                    "i64",         "isize", "u8",    "u16",
                                    "u32",         "u64",   "usize", "f32",
                                    "f64",         "bool",  "char",  "newline"};
    for (const std::string& suffix : suffixes) {
        EXPECT_NE(ir.find("@flecha_runtime_v1_stdout_" + suffix),
                  std::string::npos)
            << suffix;
    }
    EXPECT_EQ(ir.find("@write"), std::string::npos);
    EXPECT_EQ(ir.find("@snprintf"), std::string::npos);
}

TEST(LlvmCodeGeneratorTests, EmitsFormattedStandardOutput) {
    GeneratedSource result = Generate(
        "void output_byte(u8 value) { stdoutf(\"%b\", value); } "
        "void deferred(i32 value) { defer stdoutf(\"value: %d\", value); } "
        "i32 main() { stdoutf(\"expected: %d, text: %s%s, ok: %b/%b, "
        "percent: %%\", 42, \"flecha\", '!', true, 1 as u8); return 0; }");

    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.module, nullptr) << result.generator->Error();
    EXPECT_TRUE(VerifyModule(*result.module).empty());
    const std::string ir = ModuleText(*result.module);
    EXPECT_NE(ir.find("@flecha_runtime_v1_stdout_i32"), std::string::npos);
    EXPECT_NE(ir.find("@flecha_runtime_v1_stdout_string_view"),
              std::string::npos);
    EXPECT_NE(ir.find("@flecha_runtime_v1_stdout_char"), std::string::npos);
    EXPECT_NE(ir.find("@flecha_runtime_v1_stdout_bool"), std::string::npos);
    EXPECT_NE(ir.find("stdoutf.byte.bool"), std::string::npos);
    EXPECT_NE(ir.find("percent: %"), std::string::npos);
}

TEST(NativeCompilerTests, RunsVariadicAndFormattedStandardOutput) {
    GeneratedSource result = Generate(
        "i32 main() { stdout(\"answer: \", 42); stdoutln('!'); "
        "stdoutf(\"expected: %d, text: %s%s, ok: %b/%b, percent: %%\", "
        "-7, \"flecha\", '.', true, 0 as u8); return 0; }");
    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.boundProgram, nullptr);

    const std::filesystem::path temporaryDirectory =
        std::filesystem::temp_directory_path();
    const std::filesystem::path executablePath =
        temporaryDirectory / "flecha_formatted_stdout_test";
    const std::filesystem::path outputPath =
        temporaryDirectory / "flecha_formatted_stdout_test.txt";
    std::filesystem::remove(executablePath);
    std::filesystem::remove(outputPath);

    NativeCompiler compiler;
    ASSERT_TRUE(compiler.Ready()) << compiler.Error();
    ASSERT_TRUE(compiler.BuildExecutable(
        *result.boundProgram, "formatted_stdout_test", executablePath.string()))
        << compiler.Error();

    const std::string executablePathText = executablePath.string();
    const std::string outputPathText = outputPath.string();
    const llvm::StringRef arguments[] = {executablePathText};
    const std::array<std::optional<llvm::StringRef>, 3> redirects = {
        std::nullopt, llvm::StringRef(outputPathText), std::nullopt};
    EXPECT_EQ(llvm::sys::ExecuteAndWait(executablePathText, arguments,
                                        std::nullopt, redirects),
              0);

    std::ifstream output(outputPath, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(output)),
                           std::istreambuf_iterator<char>());
    EXPECT_EQ(text,
              "answer: 42!\n"
              "expected: -7, text: flecha., ok: true/false, percent: %");

    std::filesystem::remove(executablePath);
    std::filesystem::remove(outputPath);
}

TEST(LlvmCodeGeneratorTests, UsesTargetPointerWidthForSizeTypes) {
    GeneratedSource result =
        Generate("isize identity(isize value) { return value; }");
    ASSERT_NE(result.boundProgram, nullptr);

    LlvmCodeGenerator generator;
    generator.ConfigureTarget("i386-unknown-linux-gnu",
                              llvm::DataLayout("e-p:32:32"));
    std::unique_ptr<llvm::Module> module =
        generator.Generate(*result.boundProgram, "pointer_width_test");
    ASSERT_NE(module, nullptr) << generator.Error();
    const std::string ir = ModuleText(*module);
    EXPECT_NE(ir.find("define i32 @identity(i32 %value)"), std::string::npos);
}

TEST(LlvmCodeGeneratorTests, UsesTargetPointerWidthForSizeCasts) {
    GeneratedSource result =
        Generate("usize Convert(i64 value) { return value.cast<usize>; }");
    ASSERT_NE(result.boundProgram, nullptr);

    LlvmCodeGenerator generator;
    generator.ConfigureTarget("i386-unknown-linux-gnu",
                              llvm::DataLayout("e-p:32:32"));
    std::unique_ptr<llvm::Module> module =
        generator.Generate(*result.boundProgram, "size_cast_test");
    ASSERT_NE(module, nullptr) << generator.Error();
    const std::string ir = ModuleText(*module);
    EXPECT_NE(ir.find("trunc i64"), std::string::npos);
    EXPECT_NE(ir.find("to i32"), std::string::npos);
}

TEST(NativeCompilerTests, EmitsObjectAndLinksExecutableForHost) {
    GeneratedSource result = Generate(
        "i32 add(i32 left, i32 right) { return left + right; } "
        "i32 main() { return add(20, 22); }");
    ASSERT_NE(result.boundProgram, nullptr);

    const std::filesystem::path temporaryDirectory =
        std::filesystem::temp_directory_path();
    const std::filesystem::path executablePath =
        temporaryDirectory / "flecha_native_codegen_test";
    const std::filesystem::path objectPath =
        temporaryDirectory / "flecha_native_codegen_test.o";
    std::filesystem::remove(executablePath);
    std::filesystem::remove(objectPath);

    NativeCompiler compiler;
    ASSERT_TRUE(compiler.Ready()) << compiler.Error();
    EXPECT_FALSE(compiler.TargetTriple().empty());
    ASSERT_TRUE(compiler.BuildExecutable(*result.boundProgram, "native_test",
                                         executablePath.string()))
        << compiler.Error();
    EXPECT_FALSE(std::filesystem::exists(objectPath));
    EXPECT_TRUE(std::filesystem::is_regular_file(executablePath));

    const std::string executablePathText = executablePath.string();
    const llvm::StringRef arguments[] = {executablePathText};
    const int exitCode =
        llvm::sys::ExecuteAndWait(executablePathText, arguments);
    EXPECT_EQ(exitCode, 42);

    std::filesystem::remove(executablePath);
    std::filesystem::remove(objectPath);
}

TEST(NativeCompilerTests, RunsForwardAndRecursiveCalls) {
    GeneratedSource result = Generate(
        "i32 main() { return Add(Factorial(3), 36); } "
        "i32 Add(i32 left, i32 right) { return left + right; } "
        "i32 Factorial(i32 value) { if value <= 1 { return 1; } "
        "return value * Factorial(value - 1); }");
    ASSERT_NE(result.boundProgram, nullptr);

    const std::filesystem::path temporaryDirectory =
        std::filesystem::temp_directory_path();
    const std::filesystem::path executablePath =
        temporaryDirectory / "flecha_native_functions_test";
    const std::filesystem::path objectPath =
        temporaryDirectory / "flecha_native_functions_test.o";
    std::filesystem::remove(executablePath);
    std::filesystem::remove(objectPath);

    NativeCompiler compiler;
    ASSERT_TRUE(compiler.Ready()) << compiler.Error();
    ASSERT_TRUE(compiler.BuildExecutable(
        *result.boundProgram, "native_functions_test", executablePath.string()))
        << compiler.Error();

    const std::string executablePathText = executablePath.string();
    const llvm::StringRef arguments[] = {executablePathText};
    EXPECT_EQ(llvm::sys::ExecuteAndWait(executablePathText, arguments), 42);

    std::filesystem::remove(executablePath);
    std::filesystem::remove(objectPath);
}

TEST(NativeCompilerTests, RunsBorrowedReferenceAndSliceResults) {
    GeneratedSource result = Generate(R"(
        structure Pair { i32 left; i32 right; };
        (&i32, &i32) fields(&Pair pair) <- pair {
            return (&pair->left, &pair->right);
        }
        []const i32 view([]const i32 values) <- values { return values; }
        i32 main() {
            Pair pair = none;
            left, right := fields(&pair);
            *left = 20;
            *right = 22;
            values := [20, 22];
            readonly := view(values[:]);
            return pair.left + pair.right + readonly[0] - 20;
        }
    )");
    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.boundProgram, nullptr);

    const std::filesystem::path executablePath =
        std::filesystem::temp_directory_path() /
        "flecha_native_borrowed_results_test";
    std::filesystem::remove(executablePath);

    NativeCompiler compiler;
    ASSERT_TRUE(compiler.Ready()) << compiler.Error();
    ASSERT_TRUE(compiler.BuildExecutable(*result.boundProgram,
                                         "native_borrowed_results_test",
                                         executablePath.string()))
        << compiler.Error();

    const std::string executablePathText = executablePath.string();
    const llvm::StringRef arguments[] = {executablePathText};
    EXPECT_EQ(llvm::sys::ExecuteAndWait(executablePathText, arguments), 42);

    std::filesystem::remove(executablePath);
}

TEST(NativeCompilerTests, RunsMixedShortDestructuringDeclarations) {
    GeneratedSource result = Generate(R"(
        (i32, i32) pair(i32 value, i32 status) {
            return (value, status);
        }
        i32 main() {
            first, err := pair(20, 1);
            second, err := pair(22, 0);
            return first + second + err;
        }
    )");
    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.boundProgram, nullptr);

    const std::filesystem::path executablePath =
        std::filesystem::temp_directory_path() /
        "flecha_native_short_destructure_test";
    std::filesystem::remove(executablePath);

    NativeCompiler compiler;
    ASSERT_TRUE(compiler.Ready()) << compiler.Error();
    ASSERT_TRUE(compiler.BuildExecutable(*result.boundProgram,
                                         "native_short_destructure_test",
                                         executablePath.string()))
        << compiler.Error();

    const std::string executablePathText = executablePath.string();
    const llvm::StringRef arguments[] = {executablePathText};
    EXPECT_EQ(llvm::sys::ExecuteAndWait(executablePathText, arguments), 42);

    std::filesystem::remove(executablePath);
}

TEST(NativeCompilerTests, RunsOwnerBackedCheckedSlices) {
    GeneratedSource result = Generate(R"(
        structure Buffer { own *i32 data; usize length; };
        unsafe Buffer Buffer::init(own *i32 data, usize length) override {
            return build Buffer {
                self.data = data;
                self.length = length;
            };
        }
        unsafe void Buffer::tear(&self) override { free(self->data); }
        unsafe []i32 Buffer::view(&self) <- self {
            return self->data[:self->length];
        }
        i32 main() {
            unsafe {
                empty := Buffer::init(null, 0);
                emptyValues := empty.view();
                if emptyValues.len() != 0 { return 2; }
                data, err := new<[]i32>(2);
                if err != none { return 1; }
                buffer := Buffer::init(data, 2);
                values := buffer.view();
                values[0] = 42;
                return values[0];
            }
        }
    )");
    for (const auto& diagnostic : result.diagnostics.Diagnostics())
        ADD_FAILURE() << diagnostic.message;
    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.boundProgram, nullptr);

    const std::filesystem::path executablePath =
        std::filesystem::temp_directory_path() /
        "flecha_native_owner_backed_slice_test";
    std::filesystem::remove(executablePath);

    NativeCompiler compiler;
    ASSERT_TRUE(compiler.Ready()) << compiler.Error();
    ASSERT_TRUE(compiler.BuildExecutable(*result.boundProgram,
                                         "native_owner_backed_slice_test",
                                         executablePath.string()))
        << compiler.Error();
    const std::string executablePathText = executablePath.string();
    const llvm::StringRef arguments[] = {executablePathText};
    EXPECT_EQ(llvm::sys::ExecuteAndWait(executablePathText, arguments), 42);
    std::filesystem::remove(executablePath);
}

TEST(NativeCompilerTests, RunsWholeValueMovesWithExactlyOnceCleanup) {
    GeneratedSource result = Generate(R"(
        structure Box { own *i32 data; };
        unsafe Box Box::init(own *i32 data) override {
            return build Box { self.data = data; };
        }
        void Box::tear(&self) override {
            unsafe { free(self->data); }
        }
        unsafe i32 Box::read(&const self) { return *self->data; }
        Box relay(Box value) { return value; }
        (Box, string, i32) tag(Box value) {
            return (value, "moved", 20);
        }
        i32 main() {
            unsafe {
                firstData, err := new<i32>(20);
                if err != none { return 1; }
                secondData, err := new<i32>(22);
                if err != none { free(firstData); return 2; }
                discardedData, err := new<i32>(1);
                if err != none { free(firstData); free(secondData); return 4; }
                discarded := Box::init(discardedData);
                discarded;
                first := Box::init(firstData);
                second := Box::init(secondData);
                first = second;
                tagged, label, extra := tag(first);
                if label != "moved" { return 3; }
                result := relay(relay(tagged));
                return result.read() + extra;
            }
        }
    )");
    for (const auto& diagnostic : result.diagnostics.Diagnostics())
        ADD_FAILURE() << diagnostic.message;
    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.boundProgram, nullptr);

    const std::filesystem::path executablePath =
        std::filesystem::temp_directory_path() /
        "flecha_native_whole_value_moves_test";
    std::filesystem::remove(executablePath);

    NativeCompiler compiler;
    ASSERT_TRUE(compiler.Ready()) << compiler.Error();
    ASSERT_TRUE(compiler.BuildExecutable(*result.boundProgram,
                                         "native_whole_value_moves_test",
                                         executablePath.string()))
        << compiler.Error();
    const std::string executablePathText = executablePath.string();
    const llvm::StringRef arguments[] = {executablePathText};
    EXPECT_EQ(llvm::sys::ExecuteAndWait(executablePathText, arguments), 42);
    std::filesystem::remove(executablePath);
}

TEST(NativeCompilerTests, RunsMoveOnlyMatchResults) {
    GeneratedSource result = Generate(R"(
        enum Pick { Left, Right, }
        structure Box { i32 value; };
        void Box::tear(&self) override {}
        Box make(i32 value) {
            box := Box::init();
            box.value = value;
            return box;
        }
        Box choose(Pick pick, Box left, Box right) {
            return match pick {
                Pick.Left => left,
                Pick.Right => right,
            };
        }
        i32 main() {
            result := choose(Pick.Right, make(20), make(22));
            return result.value + 20;
        }
    )");
    for (const auto& diagnostic : result.diagnostics.Diagnostics())
        ADD_FAILURE() << diagnostic.message;
    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.boundProgram, nullptr);

    const std::filesystem::path executablePath =
        std::filesystem::temp_directory_path() /
        "flecha_native_move_match_test";
    std::filesystem::remove(executablePath);
    NativeCompiler compiler;
    ASSERT_TRUE(compiler.Ready()) << compiler.Error();
    ASSERT_TRUE(compiler.BuildExecutable(*result.boundProgram,
                                         "native_move_match_test",
                                         executablePath.string()))
        << compiler.Error();
    const std::string executablePathText = executablePath.string();
    const llvm::StringRef arguments[] = {executablePathText};
    EXPECT_EQ(llvm::sys::ExecuteAndWait(executablePathText, arguments), 42);
    std::filesystem::remove(executablePath);
}

TEST(NativeCompilerTests, DeferredMoveOnlyArgumentsTransferCleanupOnce) {
    GeneratedSource result = Generate(R"(
        structure Box { own *i32 data; *i32 tears; };
        unsafe Box Box::init(own *i32 data, *i32 tears) override {
            return build Box {
                self.data = data;
                self.tears = tears;
            };
        }
        void Box::tear(&self) override {
            unsafe {
                (*self->tears)++;
                free(self->data);
            }
        }
        void consume(Box value) {}

        structure Snapshot { i32 value; };
        void observe(Snapshot value, &i32 output) {
            *output = value.value;
        }
        void observe_text(string value, &i32 output) {
            if value == "before" { *output = 20; }
        }

        i32 main() {
            unsafe {
                tears := 0;
                observed := 0;
                textObserved := 0;
                {
                    snapshot := Snapshot::init();
                    snapshot.value = 20;
                    defer observe(snapshot, &observed);
                    snapshot.value = 1;

                    string word = "before";
                    defer observe_text(word, &textObserved);
                    word = "after";

                    data, err := new<i32>(42);
                    if err != none { return 1; }
                    box := Box::init(data, &raw tears);
                    defer consume(box);
                }
                if tears != 1 { return 2; }
                return observed + textObserved + 2;
            }
        }
    )");
    for (const auto& diagnostic : result.diagnostics.Diagnostics())
        ADD_FAILURE() << diagnostic.message;
    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.boundProgram, nullptr);

    const std::filesystem::path executablePath =
        std::filesystem::temp_directory_path() /
        "flecha_native_deferred_move_test";
    std::filesystem::remove(executablePath);
    NativeCompiler compiler;
    ASSERT_TRUE(compiler.Ready()) << compiler.Error();
    ASSERT_TRUE(compiler.BuildExecutable(*result.boundProgram,
                                         "native_deferred_move_test",
                                         executablePath.string()))
        << compiler.Error();
    const std::string executablePathText = executablePath.string();
    const llvm::StringRef arguments[] = {executablePathText};
    EXPECT_EQ(llvm::sys::ExecuteAndWait(executablePathText, arguments), 42);
    std::filesystem::remove(executablePath);
}

TEST(NativeCompilerTests, ZeroLengthMoveOnlyRepeatsDestroyTheirInitializer) {
    GeneratedSource result = Generate(R"(
        structure Box { *i32 tears; };
        unsafe Box Box::init(*i32 tears) override {
            return build Box { self.tears = tears; };
        }
        void Box::tear(&self) override {
            unsafe { (*self->tears)++; }
        }
        i32 main() {
            unsafe {
                tears := 0;
                zero := Box::init(&raw tears);
                empty := [zero; 0];
                if tears != 1 { return 1; }
                {
                    one := Box::init(&raw tears);
                    values := [one; 1];
                }
                if tears != 2 { return 2; }
                return 42;
            }
        }
    )");
    for (const auto& diagnostic : result.diagnostics.Diagnostics())
        ADD_FAILURE() << diagnostic.message;
    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.boundProgram, nullptr);

    const std::filesystem::path executablePath =
        std::filesystem::temp_directory_path() /
        "flecha_native_zero_length_move_repeat_test";
    std::filesystem::remove(executablePath);
    NativeCompiler compiler;
    ASSERT_TRUE(compiler.Ready()) << compiler.Error();
    ASSERT_TRUE(compiler.BuildExecutable(*result.boundProgram,
                                         "native_zero_length_move_repeat_test",
                                         executablePath.string()))
        << compiler.Error();
    const std::string executablePathText = executablePath.string();
    const llvm::StringRef arguments[] = {executablePathText};
    EXPECT_EQ(llvm::sys::ExecuteAndWait(executablePathText, arguments), 42);
    std::filesystem::remove(executablePath);
}

TEST(NativeCompilerTests, RunsEnumMatchesInMemberFunctions) {
    GeneratedSource result = Generate(R"(
        enum TokenKind: u8 { Identifier = 2, Number = 7, End = 9, }
        i32 TokenKind::score(&const self) {
            return match *self {
                TokenKind.Identifier => 10,
                TokenKind.Number => 20,
                TokenKind.End => 42,
            };
        }
        i32 main() {
            TokenKind kind = TokenKind.End;
            return kind.score();
        }
    )");
    ASSERT_NE(result.boundProgram, nullptr);

    const std::filesystem::path executablePath =
        std::filesystem::temp_directory_path() / "flecha_native_match_test";
    std::filesystem::remove(executablePath);

    NativeCompiler compiler;
    ASSERT_TRUE(compiler.Ready()) << compiler.Error();
    ASSERT_TRUE(compiler.BuildExecutable(*result.boundProgram, "native_match",
                                         executablePath.string()))
        << compiler.Error();
    const std::string executablePathText = executablePath.string();
    const llvm::StringRef arguments[] = {executablePathText};
    EXPECT_EQ(llvm::sys::ExecuteAndWait(executablePathText, arguments), 42);

    std::filesystem::remove(executablePath);
}

TEST(NativeCompilerTests, ObjectEmissionDoesNotRequireRuntimeLibrary) {
    GeneratedSource result = Generate("i32 main() { stdoutln(42); return 0; }");
    ASSERT_NE(result.boundProgram, nullptr);

    const std::filesystem::path objectPath =
        std::filesystem::temp_directory_path() /
        "flecha_object_without_runtime_test.o";
    const std::filesystem::path executablePath =
        std::filesystem::temp_directory_path() / "flecha_missing_runtime_test";
    std::filesystem::remove(objectPath);
    std::filesystem::remove(executablePath);

    NativeCompiler compiler("/missing/flecha/runtime/library.a");
    ASSERT_TRUE(compiler.EmitObject(*result.boundProgram, "object_only_test",
                                    objectPath.string()))
        << compiler.Error();
    EXPECT_TRUE(std::filesystem::is_regular_file(objectPath));
    EXPECT_FALSE(
        compiler.LinkExecutable(objectPath.string(), executablePath.string()));
    ASSERT_TRUE(compiler.LastDiagnostic().has_value());
    EXPECT_EQ(compiler.LastDiagnostic()->code,
              DiagnosticCode::RuntimeUnavailable);

    std::filesystem::remove(objectPath);
    std::filesystem::remove(executablePath);
}

TEST(NativeCompilerTests, ReportsLinkerFailuresAsStructuredDiagnostics) {
    const std::filesystem::path temporaryDirectory =
        std::filesystem::temp_directory_path();
    const std::filesystem::path missingObject =
        temporaryDirectory / "flecha_intentionally_missing_object.o";
    const std::filesystem::path executablePath =
        temporaryDirectory / "flecha_failed_link_test";
    std::filesystem::remove(missingObject);
    std::filesystem::remove(executablePath);

    NativeCompiler compiler;
    EXPECT_FALSE(compiler.LinkExecutable(missingObject.string(),
                                         executablePath.string()));
    ASSERT_TRUE(compiler.LastDiagnostic().has_value());
    EXPECT_EQ(compiler.LastDiagnostic()->code, DiagnosticCode::LinkerFailed);
    EXPECT_NE(compiler.Error().find("system linker '"), std::string::npos);
    EXPECT_NE(compiler.Error().find("failed for target '"), std::string::npos);
    EXPECT_NE(compiler.Error().find("linker output:"), std::string::npos);
    EXPECT_NE(compiler.Error().find(missingObject.filename().string()),
              std::string::npos);
    EXPECT_NE(compiler.Error().find("FLECHA_LINKER"), std::string::npos);
    EXPECT_FALSE(std::filesystem::exists(executablePath));
}

TEST(NativeCompilerTests, ReportsAttemptedDriversWhenNoLinkerIsOnPath) {
    const std::filesystem::path emptyPath =
        std::filesystem::temp_directory_path() /
        "flecha_linker_search_path_that_does_not_exist";
    ScopedEnvironment linkerOverride("FLECHA_LINKER", std::nullopt);
    ScopedEnvironment path("PATH", emptyPath.string());

    const std::filesystem::path output =
        std::filesystem::temp_directory_path() / "flecha_missing_linker_test";
    NativeCompiler compiler;
    EXPECT_FALSE(compiler.LinkExecutable("missing-object.o", output.string()));
    ASSERT_TRUE(compiler.LastDiagnostic().has_value());
    EXPECT_EQ(compiler.LastDiagnostic()->code, DiagnosticCode::LinkerFailed);
    EXPECT_NE(compiler.Error().find("unable to find a supported system C++ "
                                    "linker driver for target '"),
              std::string::npos);
    EXPECT_NE(compiler.Error().find("tried:"), std::string::npos);
    EXPECT_NE(compiler.Error().find("FLECHA_LINKER"), std::string::npos);
    EXPECT_FALSE(std::filesystem::exists(output));
}

TEST(NativeCompilerTests, HonorsConfiguredLinkerWithoutFallback) {
    constexpr const char* CONFIGURED_LINKER =
        "flecha-linker-that-does-not-exist";
    ScopedEnvironment linkerOverride("FLECHA_LINKER", CONFIGURED_LINKER);

    const std::filesystem::path output =
        std::filesystem::temp_directory_path() / "flecha_linker_override_test";
    NativeCompiler compiler;
    EXPECT_FALSE(compiler.LinkExecutable("missing-object.o", output.string()));
    ASSERT_TRUE(compiler.LastDiagnostic().has_value());
    EXPECT_EQ(compiler.LastDiagnostic()->code, DiagnosticCode::LinkerFailed);
    EXPECT_NE(compiler.Error().find(std::string("tried: ") + CONFIGURED_LINKER +
                                    ". "),
              std::string::npos);
    EXPECT_FALSE(std::filesystem::exists(output));
}

TEST(LlvmCodeGeneratorTests,
     EmitsDelayedScalarsAndNullInitializedStringStorage) {
    GeneratedSource result = Generate(
        "i32 main(bool c) { i32 value; string text; "
        "if c { return 0; } else { value = 42; text = \"ready\"; } "
        "stdoutln(text); return value; }");
    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.module, nullptr) << result.generator->Error();
    EXPECT_TRUE(VerifyModule(*result.module).empty());
    const std::string ir = ModuleText(*result.module);
    EXPECT_NE(ir.find("%value = alloca i32"), std::string::npos);
    EXPECT_NE(ir.find("zeroinitializer, ptr %text"), std::string::npos);
    EXPECT_EQ(ir.find("store i32 0, ptr %value"), std::string::npos);
}

TEST(LlvmCodeGeneratorTests, CleansLaterStringSlotsAtEarlierBackwardJumpExits) {
    GeneratedSource result = Generate(
        "i32 main() { count := 0; while count < 2 { #again "
        "if count == 1 { return 42; } count = count + 1; "
        "string late = \"owned\"; jumpto again; } return 0; }");
    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.module, nullptr) << result.generator->Error();
    EXPECT_TRUE(VerifyModule(*result.module).empty());
    const llvm::Function* function = result.module->getFunction("main");
    ASSERT_NE(function, nullptr);
    bool foundReturn = false;
    for (const llvm::BasicBlock& block : *function) {
        const auto* terminator =
            llvm::dyn_cast<llvm::ReturnInst>(block.getTerminator());
        if (terminator == nullptr) continue;
        const auto* value =
            llvm::dyn_cast<llvm::ConstantInt>(terminator->getReturnValue());
        if (value == nullptr || value->getSExtValue() != 42) continue;
        foundReturn = true;
        bool cleansLate = false;
        for (const llvm::Instruction& instruction : block) {
            const auto* store = llvm::dyn_cast<llvm::StoreInst>(&instruction);
            if (store != nullptr &&
                store->getPointerOperand()->getName() == "late") {
                cleansLate = llvm::isa<llvm::ConstantAggregateZero>(
                    store->getValueOperand());
            }
        }
        EXPECT_TRUE(cleansLate);
    }
    EXPECT_TRUE(foundReturn);
}

TEST(LlvmCodeGeneratorTests, EmitsRuntimeInitializedConstValuesAndParameters) {
    GeneratedSource result = Generate(
        "const i32 add(const i32 input) { return input + 2; } "
        "i32 main() { const i32 value = add(40); copy := value; "
        "copy = copy + 1; const i64 wide = value.cast<const i64>; "
        "const bool same = value == 42; const i32 zero = none; "
        "const string text = \"hi\"; const stringv view = text.view(); "
        "stdoutln(view); return copy - 1 + zero; }");
    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.module, nullptr) << result.generator->Error();
    EXPECT_TRUE(VerifyModule(*result.module).empty());
    const std::string ir = ModuleText(*result.module);
    EXPECT_NE(ir.find("define i32 @add(i32 %input)"), std::string::npos);
    EXPECT_NE(ir.find("call i32 @add(i32 40)"), std::string::npos);
}

TEST(LlvmCodeGeneratorTests, EmitsTypedZeroValuesForConstInitializers) {
    GeneratedSource result = Generate(
        "const i32 zero() { return none; } "
        "i32 identity(const i32 value) { return value; } "
        "i32 main() { const bool b = none; const char c = none; "
        "const f64 f = none; const string s = none; const stringv v = none; "
        "i32 mutableZero = 1; mutableZero = none; "
        "const i32 result = identity((none)) + zero(); return result; }");
    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.module, nullptr) << result.generator->Error();
    EXPECT_TRUE(VerifyModule(*result.module).empty());
}

TEST(LlvmCodeGeneratorTests,
     EmitsRawAddressesIndirectAccessAndPointerEquality) {
    GeneratedSource result = Generate(
        "unsafe i32 read(*const i32 p) { return *p; } "
        "i32 main() { i32 value = 40; *i32 p = null; "
        "unsafe { p = &raw value; (*p) = *p + 2; } "
        "if p != none and null != p { unsafe { return read(p); } } return 0; "
        "}");
    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.module, nullptr) << result.generator->Error();
    EXPECT_TRUE(VerifyModule(*result.module).empty());
    const std::string ir = ModuleText(*result.module);
    EXPECT_NE(ir.find("define i32 @read(ptr %p)"), std::string::npos);
    EXPECT_NE(ir.find("icmp ne ptr"), std::string::npos);
    EXPECT_NE(ir.find("load i32, ptr"), std::string::npos);
}

TEST(CodeGeneratorTests, EmitsCheckedReferencesAsAddressesAndReborrowingCalls) {
    GeneratedSource result = Generate(
        "void bump(&u8 r) { *r = *r + 1; }"
        "u8 read(&const u8 r) { return *r; }"
        "u8 main() { u8 x = 3; r := &x; *r = *r * 2; bump(r); return read(r); "
        "}");
    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.module, nullptr) << result.generator->Error();
    EXPECT_TRUE(VerifyModule(*result.module).empty());
    const std::string ir = ModuleText(*result.module);
    EXPECT_NE(ir.find("define void @bump(ptr %r)"), std::string::npos);
    EXPECT_NE(ir.find("define i8 @read(ptr %r)"), std::string::npos);
    EXPECT_NE(ir.find("call void @bump(ptr"), std::string::npos);
    EXPECT_NE(ir.find("mul i8"), std::string::npos);
}

TEST(CodeGeneratorTests, EmitsAggregateValuesAndBoundsCheckedElementAddresses) {
    GeneratedSource result = Generate(
        "[2]u8 make() { return [1, 2]; }"
        "[2]u8 copy([2]u8 value) { return value; }"
        "u8 access(usize i) { a := copy(make()); a[i] = 4; return a[i]; }");
    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.module, nullptr) << result.generator->Error();
    EXPECT_TRUE(VerifyModule(*result.module).empty());
    const std::string ir = ModuleText(*result.module);
    EXPECT_NE(ir.find("define [2 x i8] @make()"), std::string::npos);
    EXPECT_NE(ir.find("define [2 x i8] @copy([2 x i8] %value)"),
              std::string::npos);
    EXPECT_NE(ir.find("icmp ult i64"), std::string::npos);
    EXPECT_NE(ir.find("call void @llvm.trap()"), std::string::npos);
    EXPECT_NE(ir.find("getelementptr inbounds [2 x i8]"), std::string::npos);
}

TEST(CodeGeneratorTests, EmitsRepeatLoopWithoutDuplicatingInitializerCalls) {
    GeneratedSource result = Generate(
        "u8 once() { return 7; }"
        "u8 f(usize i) { a := [once(); 4096]; return a[i]; }"
        "usize g() { a := [once(); 0]; return a.len(); }");
    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.module, nullptr) << result.generator->Error();
    EXPECT_TRUE(VerifyModule(*result.module).empty());
    const std::string ir = ModuleText(*result.module);
    const std::size_t first = ir.find("call i8 @once()");
    const std::size_t second = ir.find("call i8 @once()", first + 1);
    ASSERT_NE(first, std::string::npos);
    ASSERT_NE(second, std::string::npos);
    EXPECT_EQ(ir.find("call i8 @once()", second + 1), std::string::npos);
    EXPECT_NE(ir.find("array.repeat.index = phi i64"), std::string::npos);
    EXPECT_LT(ir.size(), 10000);
}

TEST(CodeGeneratorTests, EmitsLoopUpdateTargetsAndSingleEvaluationIncrements) {
    GeneratedSource result = Generate(
        "usize index() { return 0; } "
        "[2]i32 values() { return [1, 2]; } "
        "i32 f() { a := [10, 20]; old := a[index()]++; "
        "loop i := 0; i < 2; i++ { proceed; } "
        "for (i, v) := range values() { proceed; } return old; }");
    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.module, nullptr) << result.generator->Error();
    EXPECT_TRUE(VerifyModule(*result.module).empty());
    const std::string ir = ModuleText(*result.module);
    EXPECT_NE(ir.find("br label %loop.update"), std::string::npos);
    const std::size_t indexCall = ir.find("call i64 @index()");
    ASSERT_NE(indexCall, std::string::npos);
    EXPECT_EQ(ir.find("call i64 @index()", indexCall + 1), std::string::npos);
    const std::size_t sourceCall = ir.find("call [2 x i32] @values()");
    ASSERT_NE(sourceCall, std::string::npos);
    EXPECT_EQ(ir.find("call [2 x i32] @values()", sourceCall + 1),
              std::string::npos);
    EXPECT_NE(ir.find("increment.old = load i32"), std::string::npos);
}

TEST(CodeGeneratorTests, EmitsSliceDescriptorsChecksAndReborrowedCalls) {
    GeneratedSource result = Generate(
        "usize size([]const u8 s) { return s.len(); }"
        "void change([]u8 s) { s.front()++; s.back() = 5; }"
        "usize f(usize start, usize end) { [4]u8 a = [1, 2, 3, 4]; "
        "s := a[start:end]; change(s); return size(s); }");
    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.module, nullptr) << result.generator->Error();
    EXPECT_TRUE(VerifyModule(*result.module).empty());
    const std::string ir = ModuleText(*result.module);
    EXPECT_NE(ir.find("{ ptr, i64 }"), std::string::npos);
    EXPECT_NE(ir.find("icmp ule i64"), std::string::npos);
    EXPECT_NE(ir.find("icmp ult i64"), std::string::npos);
    EXPECT_NE(ir.find("call void @llvm.trap()"), std::string::npos);
}

TEST(CodeGeneratorTests, EmitsDynamicDeferredRecordsAndScopeStackRestoration) {
    GeneratedSource result = Generate(
        "void f() { i := 0; while i < 3 { #again defer stdoutln(i); i++; "
        "if i < 3 { jumpto again; } } }");
    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.module, nullptr) << result.generator->Error();
    EXPECT_TRUE(VerifyModule(*result.module).empty());
    const std::string ir = ModuleText(*result.module);
    EXPECT_NE(ir.find("defer.record = alloca"), std::string::npos);
    EXPECT_NE(ir.find("@llvm.stacksave"), std::string::npos);
    EXPECT_NE(ir.find("@llvm.stackrestore"), std::string::npos);
    EXPECT_NE(ir.find("switch i32"), std::string::npos);
    EXPECT_EQ(ir.find("@malloc"), std::string::npos);
}

TEST(CodeGeneratorTests, VerifiesDeferAcrossEarlyReturnsAndScopeEntryJumps) {
    GeneratedSource result = Generate(
        "void f(bool b) { while true { jumpto inside; { defer stdoutln(1); "
        "#inside defer stdoutln(2); if b { return; } break; } } }");
    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.module, nullptr) << result.generator->Error();
    EXPECT_TRUE(VerifyModule(*result.module).empty());
}

TEST(CodeGeneratorTests, VerifiesNestedTupleReturnsArgumentsAndDestructuring) {
    auto result = Generate(R"(
        (i32, (bool, u8)) pair(i32 n) { return (n, (true, 7)); }
        (i32, (bool, u8)) pass((i32, (bool, u8)) p) { return p; }
        i32 main() { t := pass(pair(42)); (n, inner) := t; (ok, byte) := inner; return n; }
    )");
    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.module, nullptr);
    EXPECT_TRUE(VerifyModule(*result.module).empty());
    EXPECT_NE(
        ModuleText(*result.module).find("define { i32, { i1, i8 } } @pair"),
        std::string::npos);
}

TEST(CodeGeneratorTests, VerifiesReferenceAndSliceResultContracts) {
    auto result = Generate(R"(
        &i32 identity(&i32 value) <- value { return value; }
        structure Pair { i32 left; i32 right; };
        (&i32, &i32) fields(&Pair pair) <- pair {
            return (&pair->left, &pair->right);
        }
        []const i32 view([]const i32 values) <- values { return values; }
        i32 main() {
            Pair pair = none;
            left, right := fields(&pair);
            *left = 21;
            *right = 21;
            values := [20, 22];
            slice := values[:];
            readonly := view(slice);
            return pair.left + pair.right + readonly[0] + readonly[1];
        }
    )");
    for (const auto& diagnostic : result.diagnostics.Diagnostics())
        ADD_FAILURE() << diagnostic.message;
    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.module, nullptr) << result.generator->Error();
    EXPECT_TRUE(VerifyModule(*result.module).empty());
}

TEST(CodeGeneratorTests, VerifiesOwnedTupleCleanupAndDeferredCaptures) {
    auto result = Generate(R"(
        (string, i32) pair() { return ("value", 42); }
        void use((string, i32) p) { (s, n) := p; }
        i32 main() {
            i32 count = 0;
            loop ;; {
                #again
                if count == 1 { return 42; }
                t := pair();
                defer use(t);
                defer pair();
                t = pair();
                (string s, _) = t;
                (s, _) = pair();
                count++;
                jumpto again;
            }
        }
    )");
    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.module, nullptr);
    EXPECT_TRUE(VerifyModule(*result.module).empty());
}

TEST(CodeGeneratorTests,
     VerifiesErrorFamilyIdentityWideningAndTuplePropagation) {
    GeneratedSource result = Generate(
        "error Fault { Bad } error Other { Bad } "
        "(i32, Fault) named(bool bad) { if bad { return (0, Fault.Bad); } "
        "return (42, none); } "
        "(i32, error) generic(bool bad) { return named(bad); } "
        "bool equal(error a, error b) { return a == b; } "
        "bool compare() { return equal(Fault.Bad, Other.Bad); }");
    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.module, nullptr) << result.generator->Error();
    EXPECT_TRUE(VerifyModule(*result.module).empty());
    const std::string ir = ModuleText(*result.module);
    EXPECT_NE(ir.find("@.flecha.error.Fault = private constant i8 0"),
              std::string::npos);
    EXPECT_NE(ir.find("@.flecha.error.Other = private constant i8 0"),
              std::string::npos);
    EXPECT_NE(ir.find("select i1"), std::string::npos);
    EXPECT_NE(ir.find("define { i32, { ptr, i32 } } @generic"),
              std::string::npos);
}

TEST(LlvmCodeGeneratorTests, ConstructsStructureResultsInCallerStorage) {
    GeneratedSource result = Generate(
        "structure Box { i32 x = 42; }; "
        "unsafe void Box::tear(&self) override {} "
        "unsafe Box factory() { return Box::init(); } "
        "unsafe (Box, i32) pair() { return (factory(), 42); } "
        "i32 main() { unsafe { Box box = factory(); defer pair(); return "
        "box.x; } }");
    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.module, nullptr) << result.generator->Error();
    EXPECT_TRUE(VerifyModule(*result.module).empty());
    const std::string ir = ModuleText(*result.module);
    EXPECT_NE(ir.find("define void @factory(ptr"), std::string::npos);
    EXPECT_NE(ir.find("define void @pair(ptr"), std::string::npos);
    EXPECT_NE(ir.find("call void @factory(ptr"), std::string::npos);
    EXPECT_NE(ir.find("call void @pair(ptr"), std::string::npos);
}

TEST(LlvmCodeGeneratorTests, EmitsConditionalAllocationAndTypedFree) {
    GeneratedSource result = Generate(
        "structure Box { i32 x; }; unsafe void Box::tear(&self) override {} "
        "unsafe void release(*Box p) { return free(p); } "
        "i32 main() { unsafe { p, e := new<Box>(Box::init()); "
        "if e != none { return 1; } defer free(p); return p->x; } }");
    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_NE(result.module, nullptr) << result.generator->Error();
    EXPECT_TRUE(VerifyModule(*result.module).empty());
    const std::string ir = ModuleText(*result.module);
    EXPECT_NE(ir.find("@flecha_runtime_v1_alloc"), std::string::npos);
    EXPECT_NE(ir.find("@flecha_runtime_v1_free"), std::string::npos);
    EXPECT_NE(ir.find("allocation.failure"), std::string::npos);
    EXPECT_NE(ir.find("free.release"), std::string::npos);
}
