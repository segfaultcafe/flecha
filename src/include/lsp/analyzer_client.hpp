#ifndef FLECHA_LSP_ANALYZER_CLIENT_HPP
#define FLECHA_LSP_ANALYZER_CLIENT_HPP

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace flecha::lsp {

struct AnalyzerDiagnostic final {
    std::string code;
    std::filesystem::path file;
    std::size_t startByte = 0;
    std::size_t endByte = 0;
    std::string severity;
    std::string message;
};

struct AnalyzerSnapshot final {
    std::filesystem::path path;
    std::optional<std::int64_t> version;
    std::vector<AnalyzerDiagnostic> diagnostics;
};

struct AnalyzerRange final {
    std::filesystem::path file;
    std::size_t startByte = 0;
    std::size_t endByte = 0;
};

struct AnalyzerHover final {
    AnalyzerRange range;
    std::string name;
    std::string type;
    std::string kind;
};

struct AnalyzerCompletion final {
    std::string name;
    std::string type;
    std::string kind;
    AnalyzerRange declaration;
};

struct AnalyzerSemanticToken final {
    AnalyzerRange range;
    std::string kind;
    bool isDeclaration = false;
};

struct AnalyzerRename final {
    std::string name;
    std::vector<AnalyzerRange> ranges;
};

class AnalyzerClient {
   public:
    virtual ~AnalyzerClient() = default;

    virtual std::string Initialize(
        const std::filesystem::path& workspace,
        const std::optional<std::filesystem::path>& packageGraph) = 0;
    virtual void Open(const std::filesystem::path& path, std::string text,
                      std::int64_t version) = 0;
    virtual void Change(const std::filesystem::path& path, std::string text,
                        std::int64_t version) = 0;
    virtual void Close(const std::filesystem::path& path) = 0;
    virtual AnalyzerSnapshot Diagnostics(const std::filesystem::path& path) = 0;
    virtual std::optional<AnalyzerHover> Hover(
        const std::filesystem::path& path, std::size_t offset) = 0;
    virtual std::optional<AnalyzerRange> Definition(
        const std::filesystem::path& path, std::size_t offset) = 0;
    virtual std::optional<std::vector<AnalyzerRange>> References(
        const std::filesystem::path& path, std::size_t offset,
        bool includeDeclaration) = 0;
    virtual std::vector<AnalyzerCompletion> Completion(
        const std::filesystem::path& path, std::string_view query) = 0;
    virtual std::optional<AnalyzerRename> Rename(
        const std::filesystem::path& path, std::size_t offset) = 0;
    virtual std::vector<AnalyzerSemanticToken> SemanticTokens(
        const std::filesystem::path& path) = 0;
    virtual void Shutdown() = 0;
};

class SubprocessAnalyzerClient final : public AnalyzerClient {
   public:
    explicit SubprocessAnalyzerClient(std::filesystem::path compiler);
    ~SubprocessAnalyzerClient() override;

    SubprocessAnalyzerClient(const SubprocessAnalyzerClient&) = delete;
    SubprocessAnalyzerClient& operator=(const SubprocessAnalyzerClient&) =
        delete;

    std::string Initialize(
        const std::filesystem::path& workspace,
        const std::optional<std::filesystem::path>& packageGraph) override;
    void Open(const std::filesystem::path& path, std::string text,
              std::int64_t version) override;
    void Change(const std::filesystem::path& path, std::string text,
                std::int64_t version) override;
    void Close(const std::filesystem::path& path) override;
    AnalyzerSnapshot Diagnostics(const std::filesystem::path& path) override;
    std::optional<AnalyzerHover> Hover(const std::filesystem::path& path,
                                       std::size_t offset) override;
    std::optional<AnalyzerRange> Definition(const std::filesystem::path& path,
                                            std::size_t offset) override;
    std::optional<std::vector<AnalyzerRange>> References(
        const std::filesystem::path& path, std::size_t offset,
        bool includeDeclaration) override;
    std::vector<AnalyzerCompletion> Completion(
        const std::filesystem::path& path, std::string_view query) override;
    std::optional<AnalyzerRename> Rename(const std::filesystem::path& path,
                                         std::size_t offset) override;
    std::vector<AnalyzerSemanticToken> SemanticTokens(
        const std::filesystem::path& path) override;
    void Shutdown() override;

   private:
    std::string Request(std::string_view method, std::string_view params);
    void Start();
    void WriteFrame(std::string_view message);
    std::string ReadFrame();
    void CloseHandles() noexcept;

    std::filesystem::path _compiler;
    void* _process = nullptr;
    void* _input = nullptr;
    void* _output = nullptr;
    std::string _readBuffer;
    std::int64_t _nextRequestId = 1;
    bool _shutdown = false;
};

}  // namespace flecha::lsp

#endif  // FLECHA_LSP_ANALYZER_CLIENT_HPP
