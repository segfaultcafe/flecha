#ifndef FLECHA_SEMANTIC_SEMANTIC_ANALYZER_HPP
#define FLECHA_SEMANTIC_SEMANTIC_ANALYZER_HPP

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "diagnostics/diagnostic_bag.hpp"
#include "semantic_model.hpp"

namespace flecha::semantic {

class SemanticAnalyzer final {
   public:
    explicit SemanticAnalyzer(diagnostics::DiagnosticBag& diagnostics)
        : _diagnostics(diagnostics) {}

    [[nodiscard]] SemanticModel Analyze(core::CompilationUnit& unit);

   private:
    void SetSource(const std::string& id);
    void BindImports(const core::CompilationUnit& unit);
    void ValidateExports(const core::CompilationUnit& unit);
    bool ReportInaccessibleImport(const std::string& name,
                                  source::SourceRange range);
    bool ReportAmbiguousImport(const std::string& name,
                               source::SourceRange range);
    std::string TopName(const core::Declaration& declaration,
                        const std::string& name) const;
    std::unordered_map<std::string, Scope*> _moduleScopes;
    std::unordered_map<std::string, Scope*> _sourceScopes;
    std::unordered_map<std::string, std::string> _sourceModules;
    std::unordered_map<
        std::string, std::unordered_map<std::string, const core::Declaration*>>
        _inaccessibleImports;
    std::unordered_map<
        std::string,
        std::unordered_map<std::string, std::vector<const core::Declaration*>>>
        _ambiguousImports;
    std::unordered_map<std::string, std::string> _quiverNames;
    std::unordered_set<std::string> _entryModules;
    std::unordered_set<std::string> _testModules;
    std::string _currentModule;
    std::string _currentSource;
    std::string _testingModule;
    bool IsSafePlace(const core::Expression& expression) const;
    bool IsReferencePlace(const core::Expression& expression) const;
    SemanticType AnalyzeArray(const core::ArrayExpression& expression,
                              SemanticType expected);
    SemanticType AnalyzeMatch(const core::MatchExpression& expression,
                              SemanticType expected);
    SemanticType AnalyzeAnonymousFunction(
        const core::AnonymousFunctionExpression& expression,
        SemanticType expected);
    std::unique_ptr<core::TypeNode> TypeSyntax(
        SemanticType type, source::SourceRange range,
        core::OwnershipContract contract = core::OwnershipContract::None);
    void AnalyzeDestructure(const core::DestructureStatement& statement);
    SemanticType AnalyzeTuple(const core::TupleExpression& expression,
                              SemanticType expected);
    void ValidateTupleType(SemanticType type, source::SourceRange range,
                           bool allowBorrowed = false);
    void ValidateArrayType(SemanticType type, source::SourceRange range);
    SemanticType PlaceType(const core::Expression& expression) const;
    void RequireUnsafe(source::SourceRange range);
    SemanticType ResolveType(const core::TypeNode& type,
                             bool allowBorrowedTuple = false);
    SemanticType ResolveTypeSyntax(const core::TypeNode& type);
    void RegisterErrorTypes(const core::CompilationUnit& unit);
    void RegisterEnums(const core::CompilationUnit& unit);
    void DeclareCallableTypes(const core::CompilationUnit& unit);
    void ResolveCallableTypes(const core::CompilationUnit& unit);
    void RegisterModuleConstants(const core::CompilationUnit& unit);
    void AnalyzeModuleConstants(const core::CompilationUnit& unit);
    bool ValidateModuleConstantExpression(
        const core::Expression& expression,
        std::vector<const Symbol*>& dependencies) const;
    SemanticType AnalyzeMemberAccess(
        const core::MemberAccessExpression& expression);
    void RegisterStructures(const core::CompilationUnit& unit);
    void ResolveStructureFields();
    void RegisterFunction(const core::FunctionDeclaration& function,
                          bool generated = false);
    void GenerateInitializers();
    SemanticType AnalyzeBuild(const core::BuildExpression& expression);
    SemanticType AnalyzeStructureCall(
        const core::MemberCallExpression& expression, SemanticType receiver);
    void CheckTransfer(const core::Expression& expression, SemanticType type,
                       bool consumingOwnership = false);
    void WarnForNontrivialDefault(SemanticType type, source::SourceRange range);
    void ClassifyTrivialZeros(const core::CompilationUnit& unit);
    void RecordZeroDependency(SemanticType type);
    void RecordCleanupEffects(SemanticType type);
    void ValidateZeroCycles();
    void ValidateZeroEffects();
    void CheckMethodAccess(const FunctionSymbol& function,
                           source::SourceRange range);
    const StructureType* CurrentOwner() const;
    const EnumType* CurrentEnumOwner() const;
    void RegisterBuiltIns();
    void RegisterFunctions(const core::CompilationUnit& unit);
    void AnalyzeFunction(const core::FunctionDeclaration& function);
    void AnalyzeDefaultArguments(const core::FunctionDeclaration& function);
    void AnalyzeBlock(const core::BlockStatement& block, bool createScope);
    void AnalyzeStatement(const core::Statement& statement);
    void AnalyzeBranch(const core::ConditionalBranch& branch);
    void CollectPositions(const core::BlockStatement& block,
                          std::size_t loopDepth);
    SemanticType AnalyzeExpression(
        const core::Expression& expression,
        SemanticType expected = SemanticType::Error());
    SemanticType AnalyzeLiteral(const core::LiteralExpression& expression,
                                SemanticType expected = SemanticType::Error(),
                                bool negative = false);
    SemanticType AnalyzeName(const core::NameExpression& expression);
    SemanticType AnalyzeUnary(const core::UnaryExpression& expression,
                              SemanticType expected = SemanticType::Error());
    SemanticType AnalyzeBinary(const core::BinaryExpression& expression,
                               SemanticType expected = SemanticType::Error());
    SemanticType AnalyzeAssignment(
        const core::AssignmentExpression& expression);
    SemanticType AnalyzeAllocationCall(const core::CallExpression& expression,
                                       BuiltInFunction intrinsic);
    SemanticType AnalyzeHashCall(const core::CallExpression& expression);
    SemanticType AnalyzeStorageCall(const core::CallExpression& expression,
                                    BuiltInFunction intrinsic);
    SemanticType AnalyzeCall(const core::CallExpression& expression);
    SemanticType AnalyzeMemberCall(
        const core::MemberCallExpression& expression);
    SemanticType AnalyzeSlice(const core::SliceExpression& expression,
                              SemanticType expected);
    SemanticType AnalyzeIndex(const core::IndexExpression& expression);
    SemanticType AnalyzeCast(const core::CastExpression& expression);
    SemanticType AnalyzeIsOf(const core::IsOfExpression& expression);
    [[nodiscard]] bool BorrowsLocalString(
        const core::Expression& expression) const;
    const Symbol* AddSymbol(std::unique_ptr<Symbol> symbol,
                            const core::AstNode& declaration);
    Scope& AddScope(const Scope* parent);
    void Report(diagnostics::DiagnosticCode code, source::SourceRange range,
                std::string message);

    diagnostics::DiagnosticBag& _diagnostics;
    SemanticModel _model;
    Scope* _currentScope = nullptr;
    const FunctionSymbol* _currentFunction = nullptr;
    std::unordered_map<std::string, source::SourceRange> _positions;
    std::unordered_set<const Symbol*> _localStringBorrows;
    std::unordered_set<const StructureType*> _warnedDefaults;
    std::unordered_map<const StructureType*,
                       std::unordered_set<const StructureType*>>
        _zeroDependencies;
    std::unordered_map<const FunctionSymbol*,
                       std::unordered_set<const FunctionSymbol*>>
        _functionCalls;
    std::unordered_set<const FunctionSymbol*> _forbiddenZeroEffects;
    std::size_t _loopDepth = 0;
    bool _inUnsafe = false;
    bool _inZero = false;
    bool _zeroTrivialityKnown = false;
    std::unordered_set<const core::Expression*> _directConstructions;
    std::unordered_map<const core::CallableTypeDeclaration*,
                       std::shared_ptr<CallableSignature>>
        _callableSignatures;
    const Symbol* _buildSelf = nullptr;
    bool _allowBuildSelf = false;
    std::size_t _buildDepth = 0;
    bool _analyzingCallCallee = false;
};

}  // namespace flecha::semantic

#endif  // FLECHA_SEMANTIC_SEMANTIC_ANALYZER_HPP
