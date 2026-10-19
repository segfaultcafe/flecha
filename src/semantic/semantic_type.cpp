#include "semantic/semantic_type.hpp"

#include <cassert>
#include <utility>

#include "semantic/symbol.hpp"

namespace flecha::semantic {

SemanticType::SemanticType(SemanticTypeKind kind, SemanticType target,
                           bool isConst, std::uint64_t length)
    : _kind(kind),
      _isConst(isConst),
      _length(length),
      _target(std::make_shared<const SemanticType>(std::move(target))) {
    assert(IsCompound() && kind != SemanticTypeKind::Tuple);
}

SemanticType SemanticType::WithoutConst() const {
    SemanticType result = *this;
    result._isConst = false;
    return result;
}

SemanticType SemanticType::WithConst() const {
    SemanticType result = *this;
    result._isConst = true;
    return result;
}

bool SemanticType::ContainsStructure() const {
    if (_kind == SemanticTypeKind::Structure) return true;
    if (_kind == SemanticTypeKind::Array) return Target()->ContainsStructure();
    if (_kind == SemanticTypeKind::Tuple)
        for (const auto& element : Elements())
            if (element.ContainsStructure()) return true;
    return false;
}

bool SemanticType::ContainsOwnedBuffer() const {
    if (_kind == SemanticTypeKind::OwnedBuffer) return true;
    if (_kind == SemanticTypeKind::Structure) {
        if (!_structure->validLayout) return false;
        for (const auto& field : _structure->fields)
            if (field.type.ContainsOwnedBuffer()) return true;
    }
    if (_kind == SemanticTypeKind::Array)
        return Target()->ContainsOwnedBuffer();
    if (_kind == SemanticTypeKind::Tuple)
        for (const auto& element : Elements())
            if (element.ContainsOwnedBuffer()) return true;
    return false;
}

bool SemanticType::IsZeroable() const {
    if (_kind == SemanticTypeKind::Enum) return _enum->defaultValue.has_value();
    if (_kind == SemanticTypeKind::Structure)
        return _structure->validLayout && _structure->zero;
    if (_kind == SemanticTypeKind::Array) return Target()->IsZeroable();
    if (_kind == SemanticTypeKind::Tuple) {
        for (const auto& element : Elements())
            if (!element.IsZeroable()) return false;
        return true;
    }
    return IsNumeric() || _kind == SemanticTypeKind::Bool ||
           _kind == SemanticTypeKind::Char ||
           _kind == SemanticTypeKind::Pointer ||
           _kind == SemanticTypeKind::OwnedBuffer;
}

bool SemanticType::IsTriviallyZeroable() const {
    if (_kind == SemanticTypeKind::Enum)
        return _enum->defaultValue.has_value() && *_enum->defaultValue == 0;
    if (_kind == SemanticTypeKind::Structure) {
        return _structure->zero && _structure->zeroIsTrivial;
    }
    if (_kind == SemanticTypeKind::Array)
        return Target()->IsTriviallyZeroable();
    if (_kind == SemanticTypeKind::Tuple) {
        for (const auto& element : Elements())
            if (!element.IsTriviallyZeroable()) return false;
        return true;
    }
    return IsNumeric() || _kind == SemanticTypeKind::Bool ||
           _kind == SemanticTypeKind::Char ||
           _kind == SemanticTypeKind::Pointer ||
           _kind == SemanticTypeKind::OwnedBuffer;
}

bool SemanticType::IsCopyable() const {
    if (_kind == SemanticTypeKind::Function) return false;
    if (_kind == SemanticTypeKind::OwnedBuffer) return false;
    if (_kind == SemanticTypeKind::Structure) {
        if (!_structure->validLayout || _structure->tear) return false;
        for (const auto& field : _structure->fields)
            if (!field.type.IsCopyable()) return false;
    }
    if (_kind == SemanticTypeKind::Array) return Target()->IsCopyable();
    if (_kind == SemanticTypeKind::Tuple)
        for (const auto& element : Elements())
            if (!element.IsCopyable()) return false;
    return !IsBorrowed() || Target()->IsConst();
}

bool SemanticType::IsImmovable() const {
    if (_kind == SemanticTypeKind::Structure) {
        if (!_structure->validLayout) return false;
        if (_structure->tear && _structure->tear->IsUnsafe()) return true;
        for (const auto& field : _structure->fields)
            if (field.type.IsImmovable()) return true;
    }
    if (_kind == SemanticTypeKind::Array) return Target()->IsImmovable();
    if (_kind == SemanticTypeKind::Tuple)
        for (const auto& element : Elements())
            if (element.IsImmovable()) return true;
    return false;
}

bool SemanticType::OwnsStorage() const {
    // Every structure has a lifetime even when its default tear is empty.
    if (_kind == SemanticTypeKind::Structure) return true;
    if (_kind == SemanticTypeKind::Array) return Target()->OwnsStorage();
    if (_kind == SemanticTypeKind::String) return true;
    if (_kind == SemanticTypeKind::Function) return true;
    if (_kind == SemanticTypeKind::Tuple)
        for (const auto& element : Elements())
            if (element.OwnsStorage()) return true;
    return false;
}

bool SemanticType::operator==(const SemanticType& other) const {
    return _kind == other._kind && _isConst == other._isConst &&
           _family == other._family && _structure == other._structure &&
           _enum == other._enum && _length == other._length &&
           ((_signature == nullptr && other._signature == nullptr) ||
            (_signature != nullptr && other._signature != nullptr &&
             *_signature == *other._signature)) &&
           ((_elements == nullptr && other._elements == nullptr) ||
            (_elements != nullptr && other._elements != nullptr &&
             *_elements == *other._elements)) &&
           ((_target == nullptr && other._target == nullptr) ||
            (_target != nullptr && other._target != nullptr &&
             *_target == *other._target));
}

bool CallableSignature::operator==(const CallableSignature& other) const {
    return result == other.result && parameters == other.parameters &&
           parameterContracts == other.parameterContracts &&
           resultContracts == other.resultContracts &&
           borrowSourceIndex == other.borrowSourceIndex &&
           isUnsafe == other.isUnsafe;
}

SemanticType SemanticType::FromSyntax(const core::TypeNode& type) {
    if (type.TypeForm() == core::TypeKind::Named) return Error();
    if (type.TypeForm() == core::TypeKind::Tuple) {
        std::vector<SemanticType> elements;
        for (const auto& element : type.Elements())
            elements.push_back(FromSyntax(*element));
        return SemanticType(std::move(elements), type.IsConst());
    }
    if (type.TypeForm() == core::TypeKind::Slice)
        return SemanticType(SemanticTypeKind::Slice, FromSyntax(*type.Target()),
                            type.IsConst());
    if (type.TypeForm() == core::TypeKind::Array)
        return SemanticType(SemanticTypeKind::Array, FromSyntax(*type.Target()),
                            type.IsConst(), type.Length());
    if (type.TypeForm() != core::TypeKind::Primitive) {
        if (type.TypeForm() == core::TypeKind::Pointer &&
            type.Target()->TypeForm() == core::TypeKind::Slice)
            return SemanticType(SemanticTypeKind::OwnedBuffer,
                                FromSyntax(*type.Target()->Target()),
                                type.IsConst());
        return SemanticType(type.TypeForm() == core::TypeKind::Pointer
                                ? SemanticTypeKind::Pointer
                                : SemanticTypeKind::Reference,
                            FromSyntax(*type.Target()), type.IsConst());
    }
    SemanticType result = FromSyntax(type.Type());
    result._isConst = type.IsConst();
    return result;
}

std::string SemanticType::Name() const {
    if (_isConst) return "const " + WithoutConst().Name();
    switch (_kind) {
        case SemanticTypeKind::Enum:
            return _enum->name;
        case SemanticTypeKind::Structure:
            return _structure->displayName.empty() ? _structure->name
                                                   : _structure->displayName;
        case SemanticTypeKind::ErrorValue:
            return "error";
        case SemanticTypeKind::NamedError:
            return _family->name;
        case SemanticTypeKind::I8:
            return "i8";
        case SemanticTypeKind::I16:
            return "i16";
        case SemanticTypeKind::I32:
            return "i32";
        case SemanticTypeKind::I64:
            return "i64";
        case SemanticTypeKind::ISize:
            return "isize";
        case SemanticTypeKind::U8:
            return "u8";
        case SemanticTypeKind::U16:
            return "u16";
        case SemanticTypeKind::U32:
            return "u32";
        case SemanticTypeKind::U64:
            return "u64";
        case SemanticTypeKind::USize:
            return "usize";
        case SemanticTypeKind::F32:
            return "f32";
        case SemanticTypeKind::F64:
            return "f64";
        case SemanticTypeKind::Bool:
            return "bool";
        case SemanticTypeKind::Char:
            return "char";
        case SemanticTypeKind::Void:
            return "void";
        case SemanticTypeKind::String:
            return "string";
        case SemanticTypeKind::StringView:
            return "stringv";
        case SemanticTypeKind::Tuple: {
            std::string name = "(";
            for (const auto& element : Elements()) {
                if (name.size() > 1) name += ", ";
                name += element.Name();
            }
            return name + ")";
        }
        case SemanticTypeKind::Slice:
            return "[]" + _target->Name();
        case SemanticTypeKind::OwnedBuffer:
            return "*[]" + _target->Name();
        case SemanticTypeKind::Array:
            return "[" + std::to_string(_length) + "]" + _target->Name();
        case SemanticTypeKind::Pointer:
            return "*" + _target->Name();
        case SemanticTypeKind::Reference: {
            const std::string targetName = _target->Name();
            // Keep nested references distinct from the logical && token.
            return (targetName.starts_with('&') ? "& " : "&") + targetName;
        }
        case SemanticTypeKind::Null:
            return "null";
        case SemanticTypeKind::None:
            return "none";
        case SemanticTypeKind::Function:
            if (!_signature) return "function";
            {
                std::string name =
                    _signature->isUnsafe ? "unsafe func(" : "func(";
                for (std::size_t i = 0; i < _signature->parameters.size();
                     ++i) {
                    if (i != 0) name += ", ";
                    if (i < _signature->parameterContracts.size() &&
                        _signature->parameterContracts[i] ==
                            core::OwnershipContract::Own)
                        name += "own ";
                    else if (i < _signature->parameterContracts.size() &&
                             _signature->parameterContracts[i] ==
                                 core::OwnershipContract::Lend)
                        name += "lend ";
                    name += _signature->parameters[i].Name();
                }
                name += ") => (";
                if (_signature->result.Kind() == SemanticTypeKind::Tuple) {
                    for (std::size_t i = 0;
                         i < _signature->result.Elements().size(); ++i) {
                        if (i != 0) name += ", ";
                        if (i < _signature->resultContracts.size() &&
                            _signature->resultContracts[i] ==
                                core::OwnershipContract::Own)
                            name += "own ";
                        name += _signature->result.Elements()[i].Name();
                    }
                } else if (_signature->result.Kind() !=
                           SemanticTypeKind::Void) {
                    if (!_signature->resultContracts.empty() &&
                        _signature->resultContracts[0] ==
                            core::OwnershipContract::Own)
                        name += "own ";
                    name += _signature->result.Name();
                }
                return name + ")";
            }
        case SemanticTypeKind::Error:
            return "<error>";
    }
    return "<error>";
}

bool SemanticType::IsInteger() const {
    return _kind >= SemanticTypeKind::I8 && _kind <= SemanticTypeKind::USize;
}

bool SemanticType::IsFloating() const {
    return _kind == SemanticTypeKind::F32 || _kind == SemanticTypeKind::F64;
}

bool SemanticType::IsNumeric() const { return IsInteger() || IsFloating(); }

bool SemanticType::IsPrintable() const {
    return IsNumeric() || _kind == SemanticTypeKind::Bool ||
           _kind == SemanticTypeKind::Char ||
           _kind == SemanticTypeKind::String ||
           _kind == SemanticTypeKind::StringView;
}

SemanticType SemanticType::FromSyntax(core::PrimitiveType type) {
    switch (type) {
        case core::PrimitiveType::ErrorValue:
            return SemanticType(SemanticTypeKind::ErrorValue);
        case core::PrimitiveType::I8:
            return SemanticType(SemanticTypeKind::I8);
        case core::PrimitiveType::I16:
            return SemanticType(SemanticTypeKind::I16);
        case core::PrimitiveType::I32:
            return SemanticType(SemanticTypeKind::I32);
        case core::PrimitiveType::I64:
            return SemanticType(SemanticTypeKind::I64);
        case core::PrimitiveType::ISize:
            return SemanticType(SemanticTypeKind::ISize);
        case core::PrimitiveType::U8:
            return SemanticType(SemanticTypeKind::U8);
        case core::PrimitiveType::U16:
            return SemanticType(SemanticTypeKind::U16);
        case core::PrimitiveType::U32:
            return SemanticType(SemanticTypeKind::U32);
        case core::PrimitiveType::U64:
            return SemanticType(SemanticTypeKind::U64);
        case core::PrimitiveType::USize:
            return SemanticType(SemanticTypeKind::USize);
        case core::PrimitiveType::F32:
            return SemanticType(SemanticTypeKind::F32);
        case core::PrimitiveType::F64:
            return SemanticType(SemanticTypeKind::F64);
        case core::PrimitiveType::Bool:
            return SemanticType(SemanticTypeKind::Bool);
        case core::PrimitiveType::Char:
            return SemanticType(SemanticTypeKind::Char);
        case core::PrimitiveType::Void:
            return SemanticType(SemanticTypeKind::Void);
        case core::PrimitiveType::String:
            return SemanticType(SemanticTypeKind::String);
        case core::PrimitiveType::StringView:
            return SemanticType(SemanticTypeKind::StringView);
    }
    return Error();
}

bool IsAssignable(SemanticType destination, SemanticType source) {
    if (destination.IsError() || source.IsError()) return true;
    if (destination.WithoutConst() == source.WithoutConst()) return true;
    if (destination.Kind() == SemanticTypeKind::U8 &&
        source.Kind() == SemanticTypeKind::Char)
        return true;
    if (destination.Kind() == SemanticTypeKind::ErrorValue &&
        source.Kind() == SemanticTypeKind::NamedError)
        return true;
    if (destination.Kind() == SemanticTypeKind::Tuple &&
        source.Kind() == SemanticTypeKind::Tuple) {
        if (destination.Elements().size() != source.Elements().size())
            return false;
        for (std::size_t i = 0; i < destination.Elements().size(); ++i)
            if (!IsAssignable(destination.Elements()[i], source.Elements()[i]))
                return false;
        return true;
    }
    if (destination.Kind() == SemanticTypeKind::Pointer) {
        if (source.Kind() == SemanticTypeKind::Null) return true;
        if (source.Kind() == SemanticTypeKind::Pointer) {
            // Only add const at the immediate target; no nested covariance.
            return destination.Target()->WithoutConst() ==
                       source.Target()->WithoutConst() &&
                   (destination.Target()->IsConst() ||
                    !source.Target()->IsConst());
        }
        if (source.Kind() == SemanticTypeKind::OwnedBuffer)
            return destination.Target()->WithoutConst() ==
                       source.Target()->WithoutConst() &&
                   (destination.Target()->IsConst() ||
                    !source.Target()->IsConst());
    }
    if (destination.Kind() == SemanticTypeKind::OwnedBuffer &&
        source.Kind() == SemanticTypeKind::Null)
        return true;
    if (destination.Kind() == SemanticTypeKind::String &&
        source.Kind() == SemanticTypeKind::StringView) {
        return true;
    }
    if (source.Kind() == SemanticTypeKind::None &&
        destination.ContainsStructure())
        return destination.IsZeroable();
    if (source.Kind() == SemanticTypeKind::None &&
        destination.Kind() == SemanticTypeKind::Enum)
        return destination.IsZeroable();
    if (source.Kind() == SemanticTypeKind::None) {
        const auto enumsDefaultable = [](const auto& self,
                                         SemanticType type) -> bool {
            if (type.Kind() == SemanticTypeKind::Enum) return type.IsZeroable();
            if (type.Kind() == SemanticTypeKind::Array)
                return self(self, *type.Target());
            if (type.Kind() == SemanticTypeKind::Tuple)
                for (const auto& element : type.Elements())
                    if (!self(self, element)) return false;
            return true;
        };
        if (!enumsDefaultable(enumsDefaultable, destination)) return false;
    }
    return source.Kind() == SemanticTypeKind::None &&
           destination.Kind() != SemanticTypeKind::Void &&
           destination.Kind() != SemanticTypeKind::Function &&
           !destination.IsBorrowed();
}

bool IsExternAssignable(SemanticType destination, SemanticType source) {
    if (IsAssignable(destination, source)) return true;
    if (destination.Kind() != SemanticTypeKind::Pointer ||
        source.Kind() != SemanticTypeKind::Pointer)
        return false;

    const SemanticType destinationTarget = *destination.Target();
    const SemanticType sourceTarget = *source.Target();
    return destinationTarget.Kind() == SemanticTypeKind::U8 &&
           sourceTarget.Kind() == SemanticTypeKind::Char &&
           (destinationTarget.IsConst() || !sourceTarget.IsConst());
}

bool CanReborrow(SemanticType destination, SemanticType source) {
    return destination.IsBorrowed() && source.Kind() == destination.Kind() &&
           destination.Target()->WithoutConst() ==
               source.Target()->WithoutConst() &&
           (destination.Target()->IsConst() || !source.Target()->IsConst());
}

bool CanExplicitlyCast(SemanticType source, SemanticType destination) {
    if (source.IsError() || destination.IsError()) return true;
    if (source.IsCompound() || destination.IsCompound()) return false;
    if (source.WithoutConst() == destination.WithoutConst()) return true;
    if ((source.Kind() == SemanticTypeKind::StringView &&
         destination.Kind() == SemanticTypeKind::String) ||
        (source.Kind() == SemanticTypeKind::String &&
         destination.Kind() == SemanticTypeKind::StringView) ||
        (destination.Kind() == SemanticTypeKind::String &&
         (source.IsNumeric() || source.Kind() == SemanticTypeKind::Bool ||
          source.Kind() == SemanticTypeKind::Char))) {
        return true;
    }
    return source.IsNumeric() && destination.IsNumeric();
}

}  // namespace flecha::semantic
