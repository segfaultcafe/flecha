#ifndef FLECHA_SEMANTIC_SYMBOL_HPP
#define FLECHA_SEMANTIC_SYMBOL_HPP

#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "semantic_type.hpp"
#include "source/source_range.hpp"

namespace flecha::semantic {

enum class SymbolKind {
    Function,
    Parameter,
    LocalVariable,
    ModuleConstant,
    ErrorType,
    EnumType,
    StructureType,
    CallableType
};

enum class BuiltInFunction {
    None,
    Stdout,
    StdoutLine,
    StdoutFormat,
    Stderr,
    StderrLine,
    StderrFormat,
    StringFormat,
    Stdin,
    StdinLine,
    New,
    Free,
    Take,
    Hash,
    StorageNew,
    StorageInitialize,
    StorageRelocate,
    StorageExtract,
    StorageDestroy,
    StorageFree
};

class Symbol {
   public:
    Symbol(SymbolKind kind, std::string name, SemanticType type,
           source::SourceRange declarationRange, bool writable)
        : _kind(kind),
          _name(std::move(name)),
          _type(type),
          _declarationRange(declarationRange),
          _writable(writable) {}
    virtual ~Symbol() = default;

    Symbol(const Symbol&) = delete;
    Symbol& operator=(const Symbol&) = delete;
    Symbol(Symbol&&) = default;
    Symbol& operator=(Symbol&&) = default;

    [[nodiscard]] SymbolKind Kind() const { return _kind; }
    [[nodiscard]] const std::string& Name() const { return _name; }
    [[nodiscard]] SemanticType Type() const { return _type; }
    [[nodiscard]] const source::SourceRange& DeclarationRange() const {
        return _declarationRange;
    }
    [[nodiscard]] bool Writable() const { return _writable; }

   private:
    SymbolKind _kind;
    std::string _name;
    SemanticType _type;
    source::SourceRange _declarationRange;
    bool _writable;
};

class VariableSymbol final : public Symbol {
   public:
    VariableSymbol(SymbolKind kind, std::string name, SemanticType type,
                   source::SourceRange declarationRange, bool writable)
        : Symbol(kind, std::move(name), type, declarationRange, writable) {}
};

class FunctionSymbol final : public Symbol {
   public:
    FunctionSymbol(std::string name, SemanticType returnType,
                   std::vector<SemanticType> parameterTypes,
                   source::SourceRange declarationRange,
                   BuiltInFunction builtIn = BuiltInFunction::None,
                   bool isUnsafe = false, const StructureType* owner = nullptr,
                   bool isPrivate = false, bool hasReceiver = false,
                   bool isExtern = false, std::string linkName = {},
                   const EnumType* enumOwner = nullptr,
                   std::vector<std::size_t> borrowSourceIndices = {},
                   std::vector<core::OwnershipContract> parameterContracts = {},
                   const core::FunctionDeclaration* declaration = nullptr,
                   bool borrowsVariadicPack = false)
        : FunctionSymbol(std::move(name),
                         std::make_shared<const CallableSignature>(
                             CallableSignature{returnType,
                                               std::move(parameterTypes),
                                               std::move(parameterContracts),
                                               {},
                                               std::move(borrowSourceIndices),
                                               isUnsafe}),
                         declarationRange, builtIn, owner, isPrivate,
                         hasReceiver, isExtern, std::move(linkName), enumOwner,
                         declaration, borrowsVariadicPack) {}

    FunctionSymbol(std::string name,
                   std::shared_ptr<const CallableSignature> signature,
                   source::SourceRange declarationRange,
                   BuiltInFunction builtIn = BuiltInFunction::None,
                   const StructureType* owner = nullptr, bool isPrivate = false,
                   bool hasReceiver = false, bool isExtern = false,
                   std::string linkName = {},
                   const EnumType* enumOwner = nullptr,
                   const core::FunctionDeclaration* declaration = nullptr,
                   bool borrowsVariadicPack = false)
        : Symbol(SymbolKind::Function, std::move(name), SemanticType(signature),
                 declarationRange, false),
          _signature(std::move(signature)),
          _builtIn(builtIn),
          _owner(owner),
          _enumOwner(enumOwner),
          _isPrivate(isPrivate),
          _hasReceiver(hasReceiver),
          _isExtern(isExtern),
          _declaration(declaration),
          _borrowsVariadicPack(borrowsVariadicPack),
          _linkName(linkName.empty() ? Name() : std::move(linkName)) {}

    [[nodiscard]] SemanticType ReturnType() const { return _signature->result; }
    [[nodiscard]] const std::vector<SemanticType>& ParameterTypes() const {
        return _signature->parameters;
    }
    [[nodiscard]] const CallableSignature& Signature() const {
        return *_signature;
    }
    [[nodiscard]] bool IsUnsafe() const { return _signature->isUnsafe; }
    [[nodiscard]] bool IsOnce() const { return _signature->isOnce; }
    const StructureType* Owner() const { return _owner; }
    const EnumType* EnumOwner() const { return _enumOwner; }
    bool HasOwner() const { return _owner != nullptr || _enumOwner != nullptr; }
    bool IsPrivate() const { return _isPrivate; }
    bool HasReceiver() const { return _hasReceiver; }
    bool IsExtern() const { return _isExtern; }
    const std::string& LinkName() const { return _linkName; }
    [[nodiscard]] const std::vector<std::size_t>& BorrowSourceIndices() const {
        return _signature->borrowSourceIndices;
    }
    [[nodiscard]] core::OwnershipContract ParameterContract(
        std::size_t index) const {
        return index < _signature->parameterContracts.size()
                   ? _signature->parameterContracts[index]
                   : core::OwnershipContract::None;
    }
    [[nodiscard]] BuiltInFunction BuiltIn() const { return _builtIn; }
    [[nodiscard]] const core::FunctionDeclaration* Declaration() const {
        return _declaration;
    }
    [[nodiscard]] bool BorrowsVariadicPack() const {
        return _borrowsVariadicPack;
    }

   private:
    std::shared_ptr<const CallableSignature> _signature;
    BuiltInFunction _builtIn;
    const StructureType* _owner;
    const EnumType* _enumOwner;
    bool _isPrivate;
    bool _hasReceiver;
    bool _isExtern;
    const core::FunctionDeclaration* _declaration;
    bool _borrowsVariadicPack;
    std::string _linkName;
};

struct EnumVariant final {
    std::string name;
    std::uint64_t value = 0;
    source::SourceRange range;
};

struct EnumType final {
    std::string name;
    const core::EnumDeclaration* syntax = nullptr;
    SemanticType backing = SemanticType(SemanticTypeKind::U32);
    std::vector<EnumVariant> variants;
    std::optional<std::uint64_t> defaultValue;
    std::unordered_map<std::string, const FunctionSymbol*> methods;
};

struct StructureField final {
    std::string name;
    SemanticType type;
    const core::StructureField* syntax;
};

struct StructureType final {
    std::string name;
    std::string displayName;
    const core::StructureDeclaration* syntax = nullptr;
    std::vector<StructureField> fields;
    std::unordered_map<std::string, const FunctionSymbol*> methods;
    const FunctionSymbol* init = nullptr;
    const FunctionSymbol* zero = nullptr;
    const FunctionSymbol* tear = nullptr;
    bool hasCustomInit = false;
    bool zeroIsGenerated = false;
    bool zeroIsTrivial = false;
    bool validLayout = false;
};

}  // namespace flecha::semantic

#endif  // FLECHA_SEMANTIC_SYMBOL_HPP
