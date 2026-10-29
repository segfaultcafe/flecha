#ifndef FLECHA_CORE_AST_HPP
#define FLECHA_CORE_AST_HPP

#include <cassert>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "source/source_range.hpp"
#include "token.hpp"
#include "token_type.hpp"

namespace flecha::core {

enum class AstNodeKind {
    CompilationUnit,
    FunctionDeclaration,
    CallableTypeDeclaration,
    ErrorDeclaration,
    EnumDeclaration,
    TypeSetDeclaration,
    ModuleConstantDeclaration,
    StructureDeclaration,
    StructureField,
    BuildExpression,
    AnonymousCapture,
    AnonymousFunctionExpression,
    Parameter,
    Type,
    BlockStatement,
    ReturnStatement,
    DeferStatement,
    IfStatement,
    WhileStatement,
    RangeStatement,
    ProceedStatement,
    BreakStatement,
    JumpToStatement,
    PositionStatement,
    ConditionalBranch,
    AlsoClause,
    VariableDeclarationStatement,
    ExpressionStatement,
    TupleExpression,
    DestructureStatement,
    DestructureBinding,
    ArrayExpression,
    MatchExpression,
    MatchArm,
    LiteralExpression,
    NameExpression,
    ParenthesizedExpression,
    UnaryExpression,
    BinaryExpression,
    AssignmentExpression,
    CallExpression,
    MemberCallExpression,
    MemberAccessExpression,
    IndexExpression,
    SliceExpression,
    CastExpression,
    IsOfExpression,
};

class AstNode {
   public:
    explicit AstNode(source::SourceRange range) : _range(range) {}
    virtual ~AstNode() = default;

    AstNode(const AstNode&) = delete;
    AstNode& operator=(const AstNode&) = delete;
    AstNode(AstNode&&) = default;
    AstNode& operator=(AstNode&&) = default;

    [[nodiscard]] virtual AstNodeKind Kind() const = 0;
    [[nodiscard]] const source::SourceRange& Range() const { return _range; }

   private:
    source::SourceRange _range;
};

class Declaration : public AstNode {
   public:
    using AstNode::AstNode;
    std::string moduleId;
    // Source units retain independent import scopes even when they contribute
    // declarations to the same logical module.
    std::string sourceId;
    bool exported = false;
    bool quiverVisible = false;
    // The source-root-qualified directory that contains this declaration.
    // Generic nominal specializations may inherit another declaration's
    // visibility boundary, so this lives on the declaration rather than being
    // inferred exclusively from moduleId.
    std::string quiverId;
    // Token spans let specialization preserve source ranges and parser
    // desugaring.
    std::vector<std::string> genericParameters;
    std::vector<std::string> ownerParameters;
    // Constraints are type-set names keyed by the generic parameter name.
    std::map<std::string, std::string> genericConstraints;
    std::vector<Token> templateTokens;
    std::vector<std::pair<std::size_t, std::size_t>> parameterTokenRanges;
    std::size_t nameTokenIndex = 0;
    std::size_t ownerTokenIndex = 0;
    std::vector<source::SourceRange> instantiationOrigins;
    std::string specializationName;
};

class Statement : public AstNode {
   public:
    using AstNode::AstNode;
};

class Expression : public AstNode {
   public:
    using AstNode::AstNode;
};

enum class PrimitiveType {
    I8,
    I16,
    I32,
    I64,
    ISize,
    U8,
    U16,
    U32,
    U64,
    USize,
    F32,
    F64,
    Bool,
    Char,
    Void,
    String,
    StringView,
    ErrorValue,
};

enum class OwnershipContract { None, Own, Lend };

enum class TypeKind {
    Primitive,
    Named,
    Pointer,
    Reference,
    Array,
    Slice,
    Tuple
};

struct ErrorVariant final {
    std::string name;
    source::SourceRange range;
};

class ErrorDeclaration final : public Declaration {
   public:
    ErrorDeclaration(source::SourceRange range, std::string name,
                     source::SourceRange nameRange,
                     std::vector<ErrorVariant> variants)
        : Declaration(range),
          _name(std::move(name)),
          _nameRange(nameRange),
          _variants(std::move(variants)) {}
    [[nodiscard]] AstNodeKind Kind() const override {
        return AstNodeKind::ErrorDeclaration;
    }
    [[nodiscard]] const std::string& Name() const { return _name; }
    [[nodiscard]] source::SourceRange NameRange() const { return _nameRange; }
    [[nodiscard]] const std::vector<ErrorVariant>& Variants() const {
        return _variants;
    }

   private:
    std::string _name;
    source::SourceRange _nameRange;
    std::vector<ErrorVariant> _variants;
};

class MemberAccessExpression final : public Expression {
   public:
    MemberAccessExpression(source::SourceRange range,
                           std::unique_ptr<Expression> receiver,
                           std::string member, source::SourceRange memberRange,
                           TokenType access = TokenType::Dot)
        : Expression(range),
          _access(access),
          _receiver(std::move(receiver)),
          _member(std::move(member)),
          _memberRange(memberRange) {}
    [[nodiscard]] AstNodeKind Kind() const override {
        return AstNodeKind::MemberAccessExpression;
    }
    [[nodiscard]] const Expression& Receiver() const { return *_receiver; }
    [[nodiscard]] const std::string& Member() const { return _member; }
    [[nodiscard]] source::SourceRange MemberRange() const {
        return _memberRange;
    }

    TokenType Access() const { return _access; }

   private:
    TokenType _access;
    std::unique_ptr<Expression> _receiver;
    std::string _member;
    source::SourceRange _memberRange;
};

class TypeNode final : public AstNode {
   public:
    std::vector<std::unique_ptr<TypeNode>> typeArguments;

    TypeNode(source::SourceRange range, std::string name, bool isConst = false,
             source::SourceRange qualifierRange = {})
        : AstNode(range),
          _typeKind(TypeKind::Named),
          _name(std::move(name)),
          _isConst(isConst),
          _qualifierRange(qualifierRange) {}
    void Rename(std::string name) { _name = std::move(name); }
    [[nodiscard]] const std::string& Name() const { return _name; }

    TypeNode(source::SourceRange range, PrimitiveType type,
             bool isConst = false, source::SourceRange qualifierRange = {})
        : AstNode(range),
          _typeKind(TypeKind::Primitive),
          _type(type),
          _isConst(isConst),
          _qualifierRange(qualifierRange) {}

    TypeNode(source::SourceRange range, TypeKind typeKind,
             std::unique_ptr<TypeNode> target, bool isConst = false,
             source::SourceRange qualifierRange = {}, std::uint64_t length = 0)
        : AstNode(range),
          _typeKind(typeKind),
          _length(length),
          _target(std::move(target)),
          _isConst(isConst),
          _qualifierRange(qualifierRange) {
        assert(typeKind != TypeKind::Primitive && _target != nullptr);
    }

    TypeNode(source::SourceRange range,
             std::vector<std::unique_ptr<TypeNode>> elements,
             bool isConst = false, source::SourceRange qualifierRange = {})
        : AstNode(range),
          _typeKind(TypeKind::Tuple),
          _elements(std::move(elements)),
          _isConst(isConst),
          _qualifierRange(qualifierRange) {}
    [[nodiscard]] const std::vector<std::unique_ptr<TypeNode>>& Elements()
        const {
        return _elements;
    }
    [[nodiscard]] AstNodeKind Kind() const override {
        return AstNodeKind::Type;
    }
    [[nodiscard]] TypeKind TypeForm() const { return _typeKind; }
    [[nodiscard]] std::uint64_t Length() const { return _length; }
    [[nodiscard]] PrimitiveType Type() const {
        assert(_typeKind == TypeKind::Primitive);
        return _type;
    }
    OwnershipContract Contract() const { return _contract; }
    void SetContract(OwnershipContract contract) { _contract = contract; }
    [[nodiscard]] const TypeNode* Target() const { return _target.get(); }
    [[nodiscard]] bool IsConst() const { return _isConst; }
    [[nodiscard]] const source::SourceRange& QualifierRange() const {
        return _qualifierRange;
    }

   private:
    OwnershipContract _contract = OwnershipContract::None;
    TypeKind _typeKind;
    std::string _name;
    std::uint64_t _length = 0;
    PrimitiveType _type = PrimitiveType::Void;
    std::unique_ptr<TypeNode> _target;
    std::vector<std::unique_ptr<TypeNode>> _elements;
    bool _isConst;
    source::SourceRange _qualifierRange;
};

class ModuleConstantDeclaration final : public Declaration {
   public:
    ModuleConstantDeclaration(source::SourceRange range,
                              std::unique_ptr<TypeNode> type, std::string name,
                              source::SourceRange nameRange,
                              std::unique_ptr<Expression> initializer)
        : Declaration(range),
          _type(std::move(type)),
          _name(std::move(name)),
          _nameRange(nameRange),
          _initializer(std::move(initializer)) {}

    [[nodiscard]] AstNodeKind Kind() const override {
        return AstNodeKind::ModuleConstantDeclaration;
    }
    [[nodiscard]] const TypeNode& Type() const { return *_type; }
    [[nodiscard]] const std::string& Name() const { return _name; }
    [[nodiscard]] const source::SourceRange& NameRange() const {
        return _nameRange;
    }
    [[nodiscard]] const Expression& Initializer() const {
        return *_initializer;
    }

   private:
    std::unique_ptr<TypeNode> _type;
    std::string _name;
    source::SourceRange _nameRange;
    std::unique_ptr<Expression> _initializer;
};

struct EnumVariant final {
    std::string name;
    source::SourceRange range;
    bool isDefault = false;
    std::string discriminant;
    source::SourceRange discriminantRange;
};

class EnumDeclaration final : public Declaration {
   public:
    EnumDeclaration(source::SourceRange range, std::string name,
                    source::SourceRange nameRange,
                    std::unique_ptr<TypeNode> backingType,
                    std::vector<EnumVariant> variants)
        : Declaration(range),
          _name(std::move(name)),
          _nameRange(nameRange),
          _backingType(std::move(backingType)),
          _variants(std::move(variants)) {}

    [[nodiscard]] AstNodeKind Kind() const override {
        return AstNodeKind::EnumDeclaration;
    }
    [[nodiscard]] const std::string& Name() const { return _name; }
    [[nodiscard]] source::SourceRange NameRange() const { return _nameRange; }
    [[nodiscard]] const TypeNode* BackingType() const {
        return _backingType.get();
    }
    [[nodiscard]] const std::vector<EnumVariant>& Variants() const {
        return _variants;
    }

   private:
    std::string _name;
    source::SourceRange _nameRange;
    std::unique_ptr<TypeNode> _backingType;
    std::vector<EnumVariant> _variants;
};

class TypeSetDeclaration final : public Declaration {
   public:
    TypeSetDeclaration(source::SourceRange range, std::string name,
                       source::SourceRange nameRange,
                       std::vector<std::unique_ptr<TypeNode>> members)
        : Declaration(range),
          _name(std::move(name)),
          _nameRange(nameRange),
          _members(std::move(members)) {}

    [[nodiscard]] AstNodeKind Kind() const override {
        return AstNodeKind::TypeSetDeclaration;
    }
    [[nodiscard]] const std::string& Name() const { return _name; }
    [[nodiscard]] source::SourceRange NameRange() const { return _nameRange; }
    [[nodiscard]] const std::vector<std::unique_ptr<TypeNode>>& Members()
        const {
        return _members;
    }

   private:
    std::string _name;
    source::SourceRange _nameRange;
    std::vector<std::unique_ptr<TypeNode>> _members;
};

class StructureField final : public AstNode {
   public:
    StructureField(source::SourceRange range, std::unique_ptr<TypeNode> type,
                   std::string name, source::SourceRange nameRange,
                   std::unique_ptr<Expression> initializer, bool isPrivate)
        : AstNode(range),
          _type(std::move(type)),
          _name(std::move(name)),
          _nameRange(nameRange),
          _initializer(std::move(initializer)),
          _isPrivate(isPrivate) {}
    AstNodeKind Kind() const override { return AstNodeKind::StructureField; }
    const TypeNode& Type() const { return *_type; }
    const std::string& Name() const { return _name; }
    source::SourceRange NameRange() const { return _nameRange; }
    const Expression* Initializer() const { return _initializer.get(); }
    bool IsPrivate() const { return _isPrivate; }

   private:
    std::unique_ptr<TypeNode> _type;
    std::string _name;
    source::SourceRange _nameRange;
    std::unique_ptr<Expression> _initializer;
    bool _isPrivate;
};

class StructureDeclaration final : public Declaration {
   public:
    StructureDeclaration(source::SourceRange range, std::string name,
                         source::SourceRange nameRange,
                         std::vector<std::unique_ptr<StructureField>> fields)
        : Declaration(range),
          _name(std::move(name)),
          _nameRange(nameRange),
          _fields(std::move(fields)) {}
    AstNodeKind Kind() const override {
        return AstNodeKind::StructureDeclaration;
    }
    const std::string& Name() const { return _name; }
    source::SourceRange NameRange() const { return _nameRange; }
    const auto& Fields() const { return _fields; }

   private:
    std::string _name;
    source::SourceRange _nameRange;
    std::vector<std::unique_ptr<StructureField>> _fields;
};

class Parameter final : public AstNode {
   public:
    Parameter(source::SourceRange range, std::unique_ptr<TypeNode> type,
              std::string name, source::SourceRange nameRange = {},
              std::unique_ptr<Expression> defaultValue = nullptr)
        : AstNode(range),
          _type(std::move(type)),
          _name(std::move(name)),
          _nameRange(nameRange.Empty() ? range : nameRange),
          _defaultValue(std::move(defaultValue)) {}

    [[nodiscard]] AstNodeKind Kind() const override {
        return AstNodeKind::Parameter;
    }
    [[nodiscard]] const TypeNode& Type() const { return *_type; }
    void SetType(std::unique_ptr<TypeNode> type) { _type = std::move(type); }
    [[nodiscard]] const std::string& Name() const { return _name; }
    [[nodiscard]] const source::SourceRange& NameRange() const {
        return _nameRange;
    }
    [[nodiscard]] const Expression* DefaultValue() const {
        return _defaultValue.get();
    }

   private:
    std::unique_ptr<TypeNode> _type;
    std::string _name;
    source::SourceRange _nameRange;
    std::unique_ptr<Expression> _defaultValue;
};

enum class FunctionModifier : std::uint8_t {
    Private,
    Override,
    Unsafe,
    Once,
    Export,
    Extern,
    Inline,
    Comptime,
};

class BlockStatement final : public Statement {
   public:
    BlockStatement(source::SourceRange range,
                   std::vector<std::unique_ptr<Statement>> statements,
                   bool isUnsafe = false)
        : Statement(range),
          _statements(std::move(statements)),
          _isUnsafe(isUnsafe) {}

    [[nodiscard]] AstNodeKind Kind() const override {
        return AstNodeKind::BlockStatement;
    }
    [[nodiscard]] bool IsUnsafe() const { return _isUnsafe; }
    [[nodiscard]] const std::vector<std::unique_ptr<Statement>>& Statements()
        const {
        return _statements;
    }

   private:
    std::vector<std::unique_ptr<Statement>> _statements;
    bool _isUnsafe;
};

class BuildExpression final : public Expression {
   public:
    BuildExpression(source::SourceRange range, std::unique_ptr<TypeNode> type,
                    std::unique_ptr<BlockStatement> body)
        : Expression(range), _type(std::move(type)), _body(std::move(body)) {}
    AstNodeKind Kind() const override { return AstNodeKind::BuildExpression; }
    const TypeNode& Type() const { return *_type; }
    const BlockStatement& Body() const { return *_body; }

   private:
    std::unique_ptr<TypeNode> _type;
    std::unique_ptr<BlockStatement> _body;
};

class FunctionDeclaration final : public Declaration {
   public:
    FunctionDeclaration(
        source::SourceRange range, std::vector<FunctionModifier> modifiers,
        std::unique_ptr<TypeNode> returnType, std::string name,
        std::vector<std::unique_ptr<Parameter>> parameters,
        std::unique_ptr<BlockStatement> body,
        source::SourceRange nameRange = {}, std::string owner = {},
        std::vector<std::string> borrowSources = {},
        std::vector<source::SourceRange> borrowSourceRanges = {},
        std::string variadicPackName = {},
        source::SourceRange variadicPackRange = {},
        bool borrowsVariadicPack = false)
        : Declaration(range),
          _owner(std::move(owner)),
          _modifiers(std::move(modifiers)),
          _returnType(std::move(returnType)),
          _name(std::move(name)),
          _parameters(std::move(parameters)),
          _body(std::move(body)),
          _nameRange(nameRange.Empty() ? range : nameRange),
          _borrowSources(std::move(borrowSources)),
          _borrowSourceRanges(std::move(borrowSourceRanges)),
          _variadicPackName(std::move(variadicPackName)),
          _variadicPackRange(variadicPackRange),
          _borrowsVariadicPack(borrowsVariadicPack) {}

    [[nodiscard]] AstNodeKind Kind() const override {
        return AstNodeKind::FunctionDeclaration;
    }
    [[nodiscard]] const std::vector<FunctionModifier>& Modifiers() const {
        return _modifiers;
    }
    [[nodiscard]] bool IsUnsafe() const {
        for (FunctionModifier modifier : _modifiers) {
            if (modifier == FunctionModifier::Unsafe) return true;
        }
        return false;
    }
    [[nodiscard]] bool IsExtern() const {
        return HasModifier(FunctionModifier::Extern);
    }
    [[nodiscard]] bool IsTest() const { return _isTest; }
    [[nodiscard]] bool IsTestHarness() const { return _isTestHarness; }
    void MarkTest() { _isTest = true; }
    void MarkTestHarness() { _isTestHarness = true; }
    const std::string& Owner() const { return _owner; }
    bool HasModifier(FunctionModifier modifier) const {
        for (const auto item : _modifiers)
            if (item == modifier) return true;
        return false;
    }
    [[nodiscard]] const TypeNode& ReturnType() const { return *_returnType; }
    void SetReturnType(std::unique_ptr<TypeNode> type) {
        _returnType = std::move(type);
    }
    void AddModifier(FunctionModifier modifier) {
        if (!HasModifier(modifier)) _modifiers.push_back(modifier);
    }
    [[nodiscard]] const std::string& Name() const { return _name; }
    [[nodiscard]] const source::SourceRange& NameRange() const {
        return _nameRange;
    }
    [[nodiscard]] const std::vector<std::unique_ptr<Parameter>>& Parameters()
        const {
        return _parameters;
    }
    [[nodiscard]] const BlockStatement& Body() const { return *_body; }
    [[nodiscard]] bool HasBorrowSources() const {
        return !_borrowSources.empty() || _borrowsVariadicPack;
    }
    [[nodiscard]] const std::vector<std::string>& BorrowSources() const {
        return _borrowSources;
    }
    [[nodiscard]] const std::vector<source::SourceRange>& BorrowSourceRanges()
        const {
        return _borrowSourceRanges;
    }
    void SetBorrowSources(std::vector<std::string> sources,
                          std::vector<source::SourceRange> ranges) {
        _borrowSources = std::move(sources);
        _borrowSourceRanges = std::move(ranges);
    }
    [[nodiscard]] bool HasVariadicPack() const {
        return !_variadicPackName.empty();
    }
    [[nodiscard]] const std::string& VariadicPackName() const {
        return _variadicPackName;
    }
    [[nodiscard]] const source::SourceRange& VariadicPackRange() const {
        return _variadicPackRange;
    }
    [[nodiscard]] bool BorrowsVariadicPack() const {
        return _borrowsVariadicPack;
    }

   private:
    std::string _owner;
    std::vector<FunctionModifier> _modifiers;
    std::unique_ptr<TypeNode> _returnType;
    std::string _name;
    std::vector<std::unique_ptr<Parameter>> _parameters;
    std::unique_ptr<BlockStatement> _body;
    source::SourceRange _nameRange;
    std::vector<std::string> _borrowSources;
    std::vector<source::SourceRange> _borrowSourceRanges;
    std::string _variadicPackName;
    source::SourceRange _variadicPackRange;
    bool _borrowsVariadicPack;
    bool _isTest = false;
    bool _isTestHarness = false;
};

class AnonymousFunctionExpression final : public Expression {
   public:
    enum class CaptureMode { Copy, Reference, ConstReference, Lend, Own };

    class Capture final : public AstNode {
       public:
        Capture(source::SourceRange range, CaptureMode mode, std::string name,
                source::SourceRange nameRange)
            : AstNode(range),
              _mode(mode),
              _name(std::move(name)),
              _nameRange(nameRange) {}

        [[nodiscard]] AstNodeKind Kind() const override {
            return AstNodeKind::AnonymousCapture;
        }
        [[nodiscard]] CaptureMode Mode() const { return _mode; }
        [[nodiscard]] const std::string& Name() const { return _name; }
        [[nodiscard]] const source::SourceRange& NameRange() const {
            return _nameRange;
        }

       private:
        CaptureMode _mode;
        std::string _name;
        source::SourceRange _nameRange;
    };

    AnonymousFunctionExpression(source::SourceRange range,
                                std::unique_ptr<FunctionDeclaration> function,
                                std::vector<std::unique_ptr<Capture>> captures,
                                bool inferredSignature,
                                bool explicitCaptureList)
        : Expression(range),
          _function(std::move(function)),
          _captures(std::move(captures)),
          _inferredSignature(inferredSignature),
          _explicitCaptureList(explicitCaptureList) {}

    [[nodiscard]] AstNodeKind Kind() const override {
        return AstNodeKind::AnonymousFunctionExpression;
    }
    [[nodiscard]] const FunctionDeclaration& Function() const {
        return *_function;
    }
    [[nodiscard]] FunctionDeclaration& Function() { return *_function; }
    [[nodiscard]] const std::vector<std::unique_ptr<Capture>>& Captures()
        const {
        return _captures;
    }
    [[nodiscard]] bool HasInferredSignature() const {
        return _inferredSignature;
    }
    [[nodiscard]] bool HasExplicitCaptureList() const {
        return _explicitCaptureList;
    }

   private:
    std::unique_ptr<FunctionDeclaration> _function;
    std::vector<std::unique_ptr<Capture>> _captures;
    bool _inferredSignature;
    bool _explicitCaptureList;
};

class CallableTypeDeclaration final : public Declaration {
   public:
    CallableTypeDeclaration(
        source::SourceRange range, std::string name,
        source::SourceRange nameRange,
        std::vector<std::unique_ptr<Parameter>> parameters,
        std::vector<std::unique_ptr<TypeNode>> results, bool isUnsafe,
        bool isOnce, std::vector<std::string> borrowSources = {},
        std::vector<source::SourceRange> borrowSourceRanges = {})
        : Declaration(range),
          _name(std::move(name)),
          _nameRange(nameRange),
          _parameters(std::move(parameters)),
          _results(std::move(results)),
          _isUnsafe(isUnsafe),
          _isOnce(isOnce),
          _borrowSources(std::move(borrowSources)),
          _borrowSourceRanges(std::move(borrowSourceRanges)) {}

    [[nodiscard]] AstNodeKind Kind() const override {
        return AstNodeKind::CallableTypeDeclaration;
    }
    [[nodiscard]] const std::string& Name() const { return _name; }
    [[nodiscard]] const source::SourceRange& NameRange() const {
        return _nameRange;
    }
    [[nodiscard]] const std::vector<std::unique_ptr<Parameter>>& Parameters()
        const {
        return _parameters;
    }
    [[nodiscard]] const std::vector<std::unique_ptr<TypeNode>>& Results()
        const {
        return _results;
    }
    [[nodiscard]] bool IsUnsafe() const { return _isUnsafe; }
    [[nodiscard]] bool IsOnce() const { return _isOnce; }
    [[nodiscard]] bool HasBorrowSources() const {
        return !_borrowSources.empty();
    }
    [[nodiscard]] const std::vector<std::string>& BorrowSources() const {
        return _borrowSources;
    }
    [[nodiscard]] const std::vector<source::SourceRange>& BorrowSourceRanges()
        const {
        return _borrowSourceRanges;
    }

   private:
    std::string _name;
    source::SourceRange _nameRange;
    std::vector<std::unique_ptr<Parameter>> _parameters;
    std::vector<std::unique_ptr<TypeNode>> _results;
    bool _isUnsafe;
    bool _isOnce;
    std::vector<std::string> _borrowSources;
    std::vector<source::SourceRange> _borrowSourceRanges;
};

struct Import final {
    bool wildcard = false;
    bool explicitAlias = false;
    std::vector<std::string> path;
    std::string alias;
    std::string resolvedModule;
    source::SourceRange range;
};
struct Module final {
    std::string id;
    std::string sourceId;
    std::string name;
    std::string file;
    std::string quiverId;
    std::string quiverName;
    bool privateModule = false;
    bool entry = false;
    bool test = false;
    std::string testPair;
    std::vector<Import> imports;
};

class CompilationUnit final : public AstNode {
   public:
    CompilationUnit(source::SourceRange range,
                    std::vector<std::unique_ptr<Declaration>> declarations)
        : AstNode(range), _declarations(std::move(declarations)) {}

    std::vector<std::unique_ptr<Declaration>> genericTemplates;
    std::vector<std::unique_ptr<Declaration>> typeSets;
    std::map<std::string, const Declaration*> genericAliases;
    bool genericsExpanded = false;
    void AddDeclaration(std::unique_ptr<Declaration> declaration) {
        _declarations.push_back(std::move(declaration));
    }
    std::string moduleName;
    bool privateModule = false;
    std::vector<Import> imports;
    std::vector<Module> modules;
    std::string testingModule;
    std::vector<std::unique_ptr<Declaration>> TakeDeclarations() {
        return std::move(_declarations);
    }
    [[nodiscard]] AstNodeKind Kind() const override {
        return AstNodeKind::CompilationUnit;
    }
    [[nodiscard]] const std::vector<std::unique_ptr<Declaration>>&
    Declarations() const {
        return _declarations;
    }

   private:
    std::vector<std::unique_ptr<Declaration>> _declarations;
};

class ReturnStatement final : public Statement {
   public:
    ReturnStatement(source::SourceRange range,
                    std::unique_ptr<Expression> expression)
        : Statement(range), _expression(std::move(expression)) {}

    [[nodiscard]] AstNodeKind Kind() const override {
        return AstNodeKind::ReturnStatement;
    }
    [[nodiscard]] const Expression* Value() const { return _expression.get(); }

   private:
    std::unique_ptr<Expression> _expression;
};

class AlsoClause final : public AstNode {
   public:
    AlsoClause(source::SourceRange range, std::unique_ptr<Expression> condition,
               std::unique_ptr<BlockStatement> body)
        : AstNode(range),
          _condition(std::move(condition)),
          _body(std::move(body)) {}

    [[nodiscard]] AstNodeKind Kind() const override {
        return AstNodeKind::AlsoClause;
    }
    [[nodiscard]] const Expression& Condition() const { return *_condition; }
    [[nodiscard]] const BlockStatement& Body() const { return *_body; }

   private:
    std::unique_ptr<Expression> _condition;
    std::unique_ptr<BlockStatement> _body;
};

class ConditionalBranch final : public AstNode {
   public:
    ConditionalBranch(source::SourceRange range,
                      std::unique_ptr<Expression> condition,
                      std::unique_ptr<BlockStatement> body,
                      std::unique_ptr<AlsoClause> alsoClause)
        : AstNode(range),
          _condition(std::move(condition)),
          _body(std::move(body)),
          _alsoClause(std::move(alsoClause)) {}

    [[nodiscard]] AstNodeKind Kind() const override {
        return AstNodeKind::ConditionalBranch;
    }
    [[nodiscard]] const Expression& Condition() const { return *_condition; }
    [[nodiscard]] const BlockStatement& Body() const { return *_body; }
    [[nodiscard]] const AlsoClause* Also() const { return _alsoClause.get(); }

   private:
    std::unique_ptr<Expression> _condition;
    std::unique_ptr<BlockStatement> _body;
    std::unique_ptr<AlsoClause> _alsoClause;
};

class IfStatement final : public Statement {
   public:
    IfStatement(source::SourceRange range,
                std::unique_ptr<ConditionalBranch> primaryBranch,
                std::vector<std::unique_ptr<ConditionalBranch>> elseIfBranches,
                std::unique_ptr<BlockStatement> elseBody)
        : Statement(range),
          _primaryBranch(std::move(primaryBranch)),
          _elseIfBranches(std::move(elseIfBranches)),
          _elseBody(std::move(elseBody)) {}

    [[nodiscard]] AstNodeKind Kind() const override {
        return AstNodeKind::IfStatement;
    }
    [[nodiscard]] const ConditionalBranch& PrimaryBranch() const {
        return *_primaryBranch;
    }
    [[nodiscard]] const std::vector<std::unique_ptr<ConditionalBranch>>&
    ElseIfBranches() const {
        return _elseIfBranches;
    }
    [[nodiscard]] const BlockStatement* ElseBody() const {
        return _elseBody.get();
    }

   private:
    std::unique_ptr<ConditionalBranch> _primaryBranch;
    std::vector<std::unique_ptr<ConditionalBranch>> _elseIfBranches;
    std::unique_ptr<BlockStatement> _elseBody;
};

class WhileStatement final : public Statement {
   public:
    WhileStatement(source::SourceRange range,
                   std::unique_ptr<Expression> condition,
                   std::unique_ptr<BlockStatement> body,
                   std::unique_ptr<AlsoClause> alsoClause,
                   std::unique_ptr<Expression> update = nullptr)
        : Statement(range),
          _condition(std::move(condition)),
          _body(std::move(body)),
          _alsoClause(std::move(alsoClause)),
          _update(std::move(update)) {}

    [[nodiscard]] AstNodeKind Kind() const override {
        return AstNodeKind::WhileStatement;
    }
    [[nodiscard]] const Expression& Condition() const { return *_condition; }
    [[nodiscard]] const BlockStatement& Body() const { return *_body; }
    [[nodiscard]] const AlsoClause* Also() const { return _alsoClause.get(); }
    [[nodiscard]] const Expression* Update() const { return _update.get(); }

   private:
    std::unique_ptr<Expression> _condition;
    std::unique_ptr<BlockStatement> _body;
    std::unique_ptr<AlsoClause> _alsoClause;
    std::unique_ptr<Expression> _update;
};

class ProceedStatement final : public Statement {
   public:
    using Statement::Statement;
    [[nodiscard]] AstNodeKind Kind() const override {
        return AstNodeKind::ProceedStatement;
    }
};

class BreakStatement final : public Statement {
   public:
    using Statement::Statement;
    [[nodiscard]] AstNodeKind Kind() const override {
        return AstNodeKind::BreakStatement;
    }
};

class JumpToStatement final : public Statement {
   public:
    JumpToStatement(source::SourceRange range, std::string position,
                    source::SourceRange positionRange)
        : Statement(range),
          _position(std::move(position)),
          _positionRange(positionRange) {}
    [[nodiscard]] AstNodeKind Kind() const override {
        return AstNodeKind::JumpToStatement;
    }
    [[nodiscard]] const std::string& Position() const { return _position; }
    [[nodiscard]] const source::SourceRange& PositionRange() const {
        return _positionRange;
    }

   private:
    std::string _position;
    source::SourceRange _positionRange;
};

class PositionStatement final : public Statement {
   public:
    PositionStatement(source::SourceRange range, std::string name,
                      source::SourceRange nameRange)
        : Statement(range), _name(std::move(name)), _nameRange(nameRange) {}
    [[nodiscard]] AstNodeKind Kind() const override {
        return AstNodeKind::PositionStatement;
    }
    [[nodiscard]] const std::string& Name() const { return _name; }
    [[nodiscard]] const source::SourceRange& NameRange() const {
        return _nameRange;
    }

   private:
    std::string _name;
    source::SourceRange _nameRange;
};

class VariableDeclarationStatement final : public Statement {
   public:
    VariableDeclarationStatement(source::SourceRange range,
                                 std::unique_ptr<TypeNode> type,
                                 std::string name,
                                 std::unique_ptr<Expression> initializer,
                                 source::SourceRange nameRange = {},
                                 bool isRangeSource = false,
                                 bool isImplicitlyDefaulted = false)
        : Statement(range),
          _type(std::move(type)),
          _name(std::move(name)),
          _initializer(std::move(initializer)),
          _nameRange(nameRange.Empty() ? range : nameRange),
          _isRangeSource(isRangeSource),
          _isImplicitlyDefaulted(isImplicitlyDefaulted) {}

    [[nodiscard]] AstNodeKind Kind() const override {
        return AstNodeKind::VariableDeclarationStatement;
    }
    [[nodiscard]] bool IsRangeSource() const { return _isRangeSource; }
    [[nodiscard]] bool IsImplicitlyDefaulted() const {
        return _isImplicitlyDefaulted;
    }
    [[nodiscard]] const TypeNode* ExplicitType() const { return _type.get(); }
    [[nodiscard]] bool HasExplicitType() const { return _type != nullptr; }
    [[nodiscard]] const std::string& Name() const { return _name; }
    [[nodiscard]] const source::SourceRange& NameRange() const {
        return _nameRange;
    }
    [[nodiscard]] const Expression* Initializer() const {
        return _initializer.get();
    }

   private:
    std::unique_ptr<TypeNode> _type;
    std::string _name;
    std::unique_ptr<Expression> _initializer;
    source::SourceRange _nameRange;
    bool _isRangeSource;
    bool _isImplicitlyDefaulted;
};

class TupleExpression final : public Expression {
   public:
    TupleExpression(source::SourceRange range,
                    std::vector<std::unique_ptr<Expression>> elements)
        : Expression(range), _elements(std::move(elements)) {}
    [[nodiscard]] AstNodeKind Kind() const override {
        return AstNodeKind::TupleExpression;
    }
    [[nodiscard]] const std::vector<std::unique_ptr<Expression>>& Elements()
        const {
        return _elements;
    }

   private:
    std::vector<std::unique_ptr<Expression>> _elements;
};

class DestructureBinding final : public AstNode {
   public:
    DestructureBinding(source::SourceRange range, std::string name,
                       std::unique_ptr<TypeNode> type)
        : AstNode(range), _name(std::move(name)), _type(std::move(type)) {}
    [[nodiscard]] AstNodeKind Kind() const override {
        return AstNodeKind::DestructureBinding;
    }
    [[nodiscard]] const std::string& Name() const { return _name; }
    [[nodiscard]] const TypeNode* ExplicitType() const { return _type.get(); }

   private:
    std::string _name;
    std::unique_ptr<TypeNode> _type;
};

class DestructureStatement final : public Statement {
   public:
    DestructureStatement(
        source::SourceRange range,
        std::vector<std::unique_ptr<DestructureBinding>> bindings,
        std::unique_ptr<Expression> value, bool inferred)
        : Statement(range),
          _bindings(std::move(bindings)),
          _value(std::move(value)),
          _inferred(inferred) {}
    [[nodiscard]] AstNodeKind Kind() const override {
        return AstNodeKind::DestructureStatement;
    }
    [[nodiscard]] const auto& Bindings() const { return _bindings; }
    [[nodiscard]] const Expression& Value() const { return *_value; }
    [[nodiscard]] bool IsInferred() const { return _inferred; }

   private:
    std::vector<std::unique_ptr<DestructureBinding>> _bindings;
    std::unique_ptr<Expression> _value;
    bool _inferred;
};

class DeferStatement final : public Statement {
   public:
    DeferStatement(source::SourceRange range, std::unique_ptr<Expression> call)
        : Statement(range), _call(std::move(call)) {}
    [[nodiscard]] AstNodeKind Kind() const override {
        return AstNodeKind::DeferStatement;
    }
    [[nodiscard]] const Expression& Call() const { return *_call; }

   private:
    std::unique_ptr<Expression> _call;
};

class ExpressionStatement final : public Statement {
   public:
    ExpressionStatement(source::SourceRange range,
                        std::unique_ptr<Expression> expression)
        : Statement(range), _expression(std::move(expression)) {}

    [[nodiscard]] AstNodeKind Kind() const override {
        return AstNodeKind::ExpressionStatement;
    }
    [[nodiscard]] const Expression& Value() const { return *_expression; }

   private:
    std::unique_ptr<Expression> _expression;
};

class ArrayExpression final : public Expression {
   public:
    ArrayExpression(source::SourceRange range,
                    std::vector<std::unique_ptr<Expression>> elements,
                    bool repeat = false, std::uint64_t length = 0)
        : Expression(range),
          _elements(std::move(elements)),
          _repeat(repeat),
          _length(repeat ? length : _elements.size()) {}
    [[nodiscard]] AstNodeKind Kind() const override {
        return AstNodeKind::ArrayExpression;
    }
    [[nodiscard]] const std::vector<std::unique_ptr<Expression>>& Elements()
        const {
        return _elements;
    }
    [[nodiscard]] bool IsRepeat() const { return _repeat; }
    [[nodiscard]] std::uint64_t Length() const { return _length; }

   private:
    std::vector<std::unique_ptr<Expression>> _elements;
    bool _repeat;
    std::uint64_t _length;
};

class MatchArm final : public AstNode {
   public:
    MatchArm(source::SourceRange range, bool wildcard, std::string enumName,
             std::string variant, source::SourceRange patternRange,
             std::unique_ptr<Expression> value)
        : AstNode(range),
          _wildcard(wildcard),
          _enumName(std::move(enumName)),
          _variant(std::move(variant)),
          _patternRange(patternRange),
          _value(std::move(value)) {}
    [[nodiscard]] AstNodeKind Kind() const override {
        return AstNodeKind::MatchArm;
    }
    [[nodiscard]] bool IsWildcard() const { return _wildcard; }
    [[nodiscard]] const std::string& EnumName() const { return _enumName; }
    [[nodiscard]] const std::string& Variant() const { return _variant; }
    [[nodiscard]] source::SourceRange PatternRange() const {
        return _patternRange;
    }
    [[nodiscard]] const Expression& Value() const { return *_value; }

   private:
    bool _wildcard;
    std::string _enumName;
    std::string _variant;
    source::SourceRange _patternRange;
    std::unique_ptr<Expression> _value;
};

class MatchExpression final : public Expression {
   public:
    MatchExpression(source::SourceRange range,
                    std::unique_ptr<Expression> value,
                    std::vector<std::unique_ptr<MatchArm>> arms)
        : Expression(range), _value(std::move(value)), _arms(std::move(arms)) {}
    [[nodiscard]] AstNodeKind Kind() const override {
        return AstNodeKind::MatchExpression;
    }
    [[nodiscard]] const Expression& Value() const { return *_value; }
    [[nodiscard]] const std::vector<std::unique_ptr<MatchArm>>& Arms() const {
        return _arms;
    }

   private:
    std::unique_ptr<Expression> _value;
    std::vector<std::unique_ptr<MatchArm>> _arms;
};

// Range syntax is normalized into an array snapshot, a private usize cursor,
// and a while loop. Retain the source/binding metadata for range-specific
// checks.
class RangeStatement final : public Statement {
   public:
    RangeStatement(source::SourceRange range,
                   std::unique_ptr<BlockStatement> expansion,
                   const VariableDeclarationStatement* source,
                   const VariableDeclarationStatement* indexBinding)
        : Statement(range),
          _expansion(std::move(expansion)),
          _source(source),
          _indexBinding(indexBinding) {}
    [[nodiscard]] AstNodeKind Kind() const override {
        return AstNodeKind::RangeStatement;
    }
    [[nodiscard]] const BlockStatement& Expansion() const {
        return *_expansion;
    }
    [[nodiscard]] const VariableDeclarationStatement& Source() const {
        return *_source;
    }
    [[nodiscard]] const VariableDeclarationStatement* IndexBinding() const {
        return _indexBinding;
    }

   private:
    std::unique_ptr<BlockStatement> _expansion;
    const VariableDeclarationStatement* _source;
    const VariableDeclarationStatement* _indexBinding;
};

class LiteralExpression final : public Expression {
   public:
    LiteralExpression(source::SourceRange range, TokenType literalType,
                      std::string value)
        : Expression(range),
          _literalType(literalType),
          _value(std::move(value)) {}

    [[nodiscard]] AstNodeKind Kind() const override {
        return AstNodeKind::LiteralExpression;
    }
    [[nodiscard]] TokenType LiteralType() const { return _literalType; }
    [[nodiscard]] const std::string& Value() const { return _value; }

   private:
    TokenType _literalType;
    std::string _value;
};

class NameExpression final : public Expression {
   public:
    std::vector<std::unique_ptr<TypeNode>> typeArguments;

    NameExpression(source::SourceRange range, std::string name)
        : Expression(range), _name(std::move(name)) {}

    [[nodiscard]] AstNodeKind Kind() const override {
        return AstNodeKind::NameExpression;
    }
    void Rename(std::string name) { _name = std::move(name); }
    [[nodiscard]] const std::string& Name() const { return _name; }

   private:
    std::string _name;
};

class ParenthesizedExpression final : public Expression {
   public:
    ParenthesizedExpression(source::SourceRange range,
                            std::unique_ptr<Expression> expression)
        : Expression(range), _expression(std::move(expression)) {}

    [[nodiscard]] AstNodeKind Kind() const override {
        return AstNodeKind::ParenthesizedExpression;
    }
    [[nodiscard]] const Expression& Inner() const { return *_expression; }

   private:
    std::unique_ptr<Expression> _expression;
};

class CastExpression final : public Expression {
   public:
    CastExpression(source::SourceRange range,
                   std::unique_ptr<Expression> expression,
                   std::unique_ptr<TypeNode> targetType)
        : Expression(range),
          _expression(std::move(expression)),
          _targetType(std::move(targetType)) {}

    [[nodiscard]] AstNodeKind Kind() const override {
        return AstNodeKind::CastExpression;
    }
    [[nodiscard]] const Expression& Value() const { return *_expression; }
    [[nodiscard]] const TypeNode& TargetType() const { return *_targetType; }

   private:
    std::unique_ptr<Expression> _expression;
    std::unique_ptr<TypeNode> _targetType;
};

class IsOfExpression final : public Expression {
   public:
    IsOfExpression(source::SourceRange range,
                   std::unique_ptr<Expression> expression,
                   std::unique_ptr<TypeNode> target)
        : Expression(range),
          _expression(std::move(expression)),
          _target(std::move(target)) {}

    [[nodiscard]] AstNodeKind Kind() const override {
        return AstNodeKind::IsOfExpression;
    }
    [[nodiscard]] const Expression& Value() const { return *_expression; }
    [[nodiscard]] const TypeNode& Target() const { return *_target; }
    [[nodiscard]] TypeNode& Target() { return *_target; }
    [[nodiscard]] const std::vector<std::unique_ptr<TypeNode>>& Candidates()
        const {
        return _candidates;
    }
    void SetCandidates(std::vector<std::unique_ptr<TypeNode>> candidates,
                       bool typeSetQuery) {
        _candidates = std::move(candidates);
        _typeSetQuery = typeSetQuery;
    }
    [[nodiscard]] bool IsTypeSetQuery() const { return _typeSetQuery; }
    void SetResult(bool result) { _result = result; }
    [[nodiscard]] bool Result() const {
        assert(_result.has_value());
        return *_result;
    }

   private:
    std::unique_ptr<Expression> _expression;
    std::unique_ptr<TypeNode> _target;
    std::vector<std::unique_ptr<TypeNode>> _candidates;
    bool _typeSetQuery = false;
    std::optional<bool> _result;
};

class UnaryExpression final : public Expression {
   public:
    UnaryExpression(source::SourceRange range, TokenType operatorType,
                    std::unique_ptr<Expression> operand,
                    bool referenceDeref = false)
        : Expression(range),
          _operatorType(operatorType),
          _operand(std::move(operand)),
          _referenceDeref(referenceDeref) {}

    [[nodiscard]] AstNodeKind Kind() const override {
        return AstNodeKind::UnaryExpression;
    }
    [[nodiscard]] TokenType OperatorType() const { return _operatorType; }
    [[nodiscard]] const Expression& Operand() const { return *_operand; }
    [[nodiscard]] bool IsReferenceDeref() const { return _referenceDeref; }

   private:
    TokenType _operatorType;
    std::unique_ptr<Expression> _operand;
    bool _referenceDeref;
};

class BinaryExpression final : public Expression {
   public:
    BinaryExpression(source::SourceRange range,
                     std::unique_ptr<Expression> left, TokenType operatorType,
                     std::unique_ptr<Expression> right)
        : Expression(range),
          _left(std::move(left)),
          _operatorType(operatorType),
          _right(std::move(right)) {}

    [[nodiscard]] AstNodeKind Kind() const override {
        return AstNodeKind::BinaryExpression;
    }
    [[nodiscard]] const Expression& Left() const { return *_left; }
    [[nodiscard]] TokenType OperatorType() const { return _operatorType; }
    [[nodiscard]] const Expression& Right() const { return *_right; }

   private:
    std::unique_ptr<Expression> _left;
    TokenType _operatorType;
    std::unique_ptr<Expression> _right;
};

class AssignmentExpression final : public Expression {
   public:
    AssignmentExpression(source::SourceRange range,
                         std::unique_ptr<Expression> target,
                         std::unique_ptr<Expression> value,
                         TokenType operatorType = TokenType::Assign)
        : Expression(range),
          _target(std::move(target)),
          _value(std::move(value)),
          _operatorType(operatorType) {}

    [[nodiscard]] AstNodeKind Kind() const override {
        return AstNodeKind::AssignmentExpression;
    }
    [[nodiscard]] const Expression& Target() const { return *_target; }
    [[nodiscard]] const Expression& Value() const { return *_value; }
    [[nodiscard]] TokenType OperatorType() const { return _operatorType; }

   private:
    std::unique_ptr<Expression> _target;
    std::unique_ptr<Expression> _value;
    TokenType _operatorType;
};

class CallExpression final : public Expression {
   public:
    CallExpression(source::SourceRange range,
                   std::unique_ptr<Expression> callee,
                   std::vector<std::unique_ptr<Expression>> arguments,
                   std::unique_ptr<TypeNode> typeArgument = nullptr,
                   std::vector<bool> expandedArguments = {})
        : Expression(range),
          _callee(std::move(callee)),
          _arguments(std::move(arguments)),
          _typeArgument(std::move(typeArgument)),
          _expandedArguments(std::move(expandedArguments)) {}

    [[nodiscard]] AstNodeKind Kind() const override {
        return AstNodeKind::CallExpression;
    }
    [[nodiscard]] const Expression& Callee() const { return *_callee; }
    [[nodiscard]] const TypeNode* TypeArgument() const {
        return _typeArgument.get();
    }
    [[nodiscard]] const std::vector<std::unique_ptr<Expression>>& Arguments()
        const {
        return _arguments;
    }
    [[nodiscard]] bool ArgumentIsExpanded(std::size_t index) const {
        return index < _expandedArguments.size() && _expandedArguments[index];
    }

   private:
    std::unique_ptr<Expression> _callee;
    std::vector<std::unique_ptr<Expression>> _arguments;
    std::unique_ptr<TypeNode> _typeArgument;
    std::vector<bool> _expandedArguments;
};

class MemberCallExpression final : public Expression {
   public:
    std::vector<std::unique_ptr<TypeNode>> typeArguments;

    MemberCallExpression(source::SourceRange range,
                         std::unique_ptr<Expression> receiver,
                         std::string member, source::SourceRange memberRange,
                         std::vector<std::unique_ptr<Expression>> arguments,
                         TokenType access = TokenType::Dot,
                         std::vector<bool> expandedArguments = {})
        : Expression(range),
          _access(access),
          _receiver(std::move(receiver)),
          _member(std::move(member)),
          _memberRange(memberRange),
          _arguments(std::move(arguments)),
          _expandedArguments(std::move(expandedArguments)) {}

    [[nodiscard]] AstNodeKind Kind() const override {
        return AstNodeKind::MemberCallExpression;
    }
    [[nodiscard]] const Expression& Receiver() const { return *_receiver; }
    [[nodiscard]] const std::string& Member() const { return _member; }
    [[nodiscard]] const source::SourceRange& MemberRange() const {
        return _memberRange;
    }
    [[nodiscard]] const std::vector<std::unique_ptr<Expression>>& Arguments()
        const {
        return _arguments;
    }
    [[nodiscard]] bool ArgumentIsExpanded(std::size_t index) const {
        return index < _expandedArguments.size() && _expandedArguments[index];
    }

    void Rename(std::string name) { _member = std::move(name); }
    TokenType Access() const { return _access; }

   private:
    TokenType _access;
    std::unique_ptr<Expression> _receiver;
    std::string _member;
    source::SourceRange _memberRange;
    std::vector<std::unique_ptr<Expression>> _arguments;
    std::vector<bool> _expandedArguments;
};

class SliceExpression final : public Expression {
   public:
    SliceExpression(source::SourceRange range,
                    std::unique_ptr<Expression> receiver,
                    std::unique_ptr<Expression> start,
                    std::unique_ptr<Expression> end)
        : Expression(range),
          _receiver(std::move(receiver)),
          _start(std::move(start)),
          _end(std::move(end)) {}
    [[nodiscard]] AstNodeKind Kind() const override {
        return AstNodeKind::SliceExpression;
    }
    [[nodiscard]] const Expression& Receiver() const { return *_receiver; }
    [[nodiscard]] const Expression* Start() const { return _start.get(); }
    [[nodiscard]] const Expression* End() const { return _end.get(); }

   private:
    std::unique_ptr<Expression> _receiver;
    std::unique_ptr<Expression> _start;
    std::unique_ptr<Expression> _end;
};

class IndexExpression final : public Expression {
   public:
    IndexExpression(source::SourceRange range,
                    std::unique_ptr<Expression> receiver,
                    std::unique_ptr<Expression> index,
                    bool isRangeElement = false)
        : Expression(range),
          _receiver(std::move(receiver)),
          _index(std::move(index)),
          _isRangeElement(isRangeElement) {}

    [[nodiscard]] AstNodeKind Kind() const override {
        return AstNodeKind::IndexExpression;
    }
    [[nodiscard]] const Expression& Receiver() const { return *_receiver; }
    [[nodiscard]] const Expression& Index() const { return *_index; }
    [[nodiscard]] bool IsRangeElement() const { return _isRangeElement; }

   private:
    std::unique_ptr<Expression> _receiver;
    std::unique_ptr<Expression> _index;
    bool _isRangeElement;
};

}  // namespace flecha::core

#endif  // FLECHA_CORE_AST_HPP
