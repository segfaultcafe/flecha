#ifndef FLECHA_SEMANTIC_SEMANTIC_TYPE_HPP
#define FLECHA_SEMANTIC_SEMANTIC_TYPE_HPP

#include <cassert>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/ast.hpp"

namespace flecha::semantic {

enum class SemanticTypeKind {
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
    Pointer,
    OwnedBuffer,
    OwnedSlice,
    Reference,
    Array,
    Slice,
    Tuple,
    Null,
    None,
    Function,
    Error,
    ErrorValue,
    NamedError,
    Enum,
    Structure,
};

struct ErrorFamily final {
    std::string name;
    std::string displayName;
    std::vector<std::string> variants;
};

struct StructureType;
struct EnumType;
struct CallableSignature;

class SemanticType final {
   public:
    explicit SemanticType(SemanticTypeKind kind, bool isConst = false)
        : _kind(kind), _isConst(isConst) {
        assert(!IsCompound());
    }
    SemanticType(SemanticTypeKind kind, SemanticType target,
                 bool isConst = false, std::uint64_t length = 0);

    explicit SemanticType(std::vector<SemanticType> elements,
                          bool isConst = false)
        : _kind(SemanticTypeKind::Tuple),
          _isConst(isConst),
          _elements(std::make_shared<const std::vector<SemanticType>>(
              std::move(elements))) {}
    [[nodiscard]] const std::vector<SemanticType>& Elements() const {
        return *_elements;
    }
    explicit SemanticType(std::shared_ptr<const ErrorFamily> family,
                          bool isConst = false)
        : _kind(SemanticTypeKind::NamedError),
          _isConst(isConst),
          _family(std::move(family)) {}
    [[nodiscard]] const ErrorFamily& Family() const { return *_family; }
    [[nodiscard]] bool IsErrorValue() const {
        return _kind == SemanticTypeKind::ErrorValue ||
               _kind == SemanticTypeKind::NamedError;
    }
    explicit SemanticType(const StructureType* structure, bool isConst = false)
        : _kind(SemanticTypeKind::Structure),
          _isConst(isConst),
          _structure(structure) {}
    const StructureType* Structure() const { return _structure; }
    explicit SemanticType(const EnumType* enumeration, bool isConst = false)
        : _kind(SemanticTypeKind::Enum),
          _isConst(isConst),
          _enum(enumeration) {}
    const EnumType* Enum() const { return _enum; }
    explicit SemanticType(std::shared_ptr<const CallableSignature> signature,
                          bool isConst = false)
        : _kind(SemanticTypeKind::Function),
          _isConst(isConst),
          _signature(std::move(signature)) {}
    [[nodiscard]] const CallableSignature& Signature() const {
        assert(_kind == SemanticTypeKind::Function && _signature);
        return *_signature;
    }
    bool ContainsStructure() const;
    bool ContainsOwnedBuffer() const;
    bool ContainsOwnedSlice() const;
    bool IsZeroable() const;
    bool IsTriviallyZeroable() const;
    bool IsCopyable() const;
    bool IsImmovable() const;
    [[nodiscard]] bool OwnsStorage() const;
    [[nodiscard]] SemanticTypeKind Kind() const { return _kind; }
    [[nodiscard]] std::string Name() const;
    [[nodiscard]] bool IsInteger() const;
    [[nodiscard]] bool IsFloating() const;
    [[nodiscard]] bool IsNumeric() const;
    [[nodiscard]] bool IsPrintable() const;
    [[nodiscard]] bool IsError() const {
        return _kind == SemanticTypeKind::Error;
    }

    [[nodiscard]] bool IsBorrowed() const {
        return _kind == SemanticTypeKind::Reference ||
               _kind == SemanticTypeKind::Slice;
    }
    [[nodiscard]] bool HasBorrowedLifetime() const {
        return IsBorrowed() || _kind == SemanticTypeKind::StringView;
    }
    [[nodiscard]] bool IsSequence() const {
        return _kind == SemanticTypeKind::Array ||
               _kind == SemanticTypeKind::Slice ||
               _kind == SemanticTypeKind::OwnedBuffer ||
               _kind == SemanticTypeKind::OwnedSlice;
    }
    [[nodiscard]] bool IsConst() const { return _isConst; }
    [[nodiscard]] bool IsCompound() const {
        return _kind == SemanticTypeKind::Pointer ||
               _kind == SemanticTypeKind::OwnedBuffer ||
               _kind == SemanticTypeKind::OwnedSlice ||
               _kind == SemanticTypeKind::Reference ||
               _kind == SemanticTypeKind::Array ||
               _kind == SemanticTypeKind::Slice ||
               _kind == SemanticTypeKind::Tuple;
    }
    [[nodiscard]] std::uint64_t Length() const { return _length; }
    [[nodiscard]] const SemanticType* Target() const { return _target.get(); }
    [[nodiscard]] SemanticType WithoutConst() const;
    [[nodiscard]] SemanticType WithConst() const;
    bool operator==(const SemanticType& other) const;

    [[nodiscard]] static SemanticType FromSyntax(const core::TypeNode& type);

    [[nodiscard]] static SemanticType FromSyntax(core::PrimitiveType type);
    [[nodiscard]] static SemanticType Error() {
        return SemanticType(SemanticTypeKind::Error);
    }

   private:
    SemanticTypeKind _kind;
    bool _isConst;
    std::shared_ptr<const ErrorFamily> _family;
    const StructureType* _structure = nullptr;
    const EnumType* _enum = nullptr;
    std::shared_ptr<const CallableSignature> _signature;
    std::uint64_t _length = 0;
    std::shared_ptr<const SemanticType> _target;
    std::shared_ptr<const std::vector<SemanticType>> _elements;
};

struct CallableSignature final {
    SemanticType result{SemanticTypeKind::Void};
    std::vector<SemanticType> parameters;
    std::vector<core::OwnershipContract> parameterContracts;
    std::vector<core::OwnershipContract> resultContracts;
    std::vector<std::size_t> borrowSourceIndices;
    bool isUnsafe = false;
    bool isOnce = false;

    bool operator==(const CallableSignature& other) const;
};

[[nodiscard]] bool IsAssignable(SemanticType destination, SemanticType source);
[[nodiscard]] bool IsExternAssignable(SemanticType destination,
                                      SemanticType source);
[[nodiscard]] bool CanReborrow(SemanticType destination, SemanticType source);
[[nodiscard]] bool CanExplicitlyCast(SemanticType source,
                                     SemanticType destination);

}  // namespace flecha::semantic

#endif  // FLECHA_SEMANTIC_SEMANTIC_TYPE_HPP
