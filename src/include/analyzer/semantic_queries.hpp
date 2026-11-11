#ifndef FLECHA_ANALYZER_SEMANTIC_QUERIES_HPP
#define FLECHA_ANALYZER_SEMANTIC_QUERIES_HPP

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "semantic/symbol.hpp"
#include "source/source_range.hpp"

namespace flecha::semantic {
class SemanticModel;
}

namespace flecha::analyzer {

enum class SemanticItemKind {
    Function,
    Parameter,
    LocalVariable,
    ModuleConstant,
    ErrorType,
    EnumType,
    StructureType,
    InterfaceType,
    CallableType,
    Field,
    Expression,
};

struct SemanticHover final {
    source::SourceRange range;
    std::string name;
    std::string type;
    SemanticItemKind kind = SemanticItemKind::Expression;
};

struct SemanticSymbol final {
    std::string name;
    std::string type;
    SemanticItemKind kind = SemanticItemKind::Expression;
    source::SourceRange declarationRange;
};

struct SemanticOccurrence final {
    source::SourceRange range;
    std::string name;
    std::string type;
    SemanticItemKind kind = SemanticItemKind::Expression;
    source::SourceRange declarationRange;
    bool isDeclaration = false;
};

struct SemanticRename final {
    std::string name;
    std::vector<source::SourceRange> ranges;
};

// Read-only queries over one fully analyzed semantic model. Positions use the
// byte offsets stored by SourceLocation; ranges are half-open.
class SemanticQueries final {
   public:
    explicit SemanticQueries(const semantic::SemanticModel& model)
        : _model(model) {}

    [[nodiscard]] std::optional<SemanticHover> Hover(
        const std::filesystem::path& file, std::size_t offset) const;
    [[nodiscard]] std::optional<SemanticOccurrence> OccurrenceAt(
        const std::filesystem::path& file, std::size_t offset) const;
    [[nodiscard]] std::vector<SemanticOccurrence> Occurrences(
        const std::filesystem::path& file) const;
    [[nodiscard]] std::optional<source::SourceRange> Definition(
        const std::filesystem::path& file, std::size_t offset) const;
    [[nodiscard]] std::optional<std::vector<source::SourceRange>> References(
        const std::filesystem::path& file, std::size_t offset,
        bool includeDeclaration = true) const;
    [[nodiscard]] std::optional<SemanticRename> Rename(
        const std::filesystem::path& file, std::size_t offset,
        bool includeDeclaration = true) const;
    [[nodiscard]] std::vector<SemanticSymbol> Symbols(
        std::string_view query = {}) const;

   private:
    enum class TargetKind { None, Symbol, Field };

    struct Match final {
        source::SourceRange range;
        source::SourceRange declarationRange;
        std::string name;
        std::string type;
        SemanticItemKind kind = SemanticItemKind::Expression;
        TargetKind targetKind = TargetKind::None;
        const void* target = nullptr;
    };

    [[nodiscard]] std::optional<Match> Find(const std::filesystem::path& file,
                                            std::size_t offset,
                                            bool includeExpressions) const;
    [[nodiscard]] std::vector<source::SourceRange> ReferenceRanges(
        const Match& match, bool includeDeclaration) const;

    const semantic::SemanticModel& _model;
};

}  // namespace flecha::analyzer

#endif  // FLECHA_ANALYZER_SEMANTIC_QUERIES_HPP
