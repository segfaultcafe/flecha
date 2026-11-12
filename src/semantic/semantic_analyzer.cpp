#include "semantic/semantic_analyzer.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "core/integer_literal.hpp"
#include "core/output_format.hpp"
#include "core/token_type.hpp"
#include "semantic/allocation_checker.hpp"
#include "semantic/borrow_checker.hpp"
#include "semantic/definite_assignment.hpp"
#include "semantic/generic_expander.hpp"
#include "semantic/method_contract.hpp"

namespace flecha::semantic {
namespace {

std::string EffectName(std::string_view name, bool crashable) {
    return std::string(name) + (crashable ? "!" : "");
}

std::string TypeMismatch(SemanticType expected, SemanticType actual) {
    return "expected '" + std::string(expected.Name()) + "', found '" +
           std::string(actual.Name()) + "'";
}

std::string MangleType(SemanticType type) {
    std::string result = type.IsConst() ? "K" : "N";
    result += std::to_string(static_cast<int>(type.Kind()));
    if (type.Kind() == SemanticTypeKind::Structure ||
        type.Kind() == SemanticTypeKind::Enum ||
        type.Kind() == SemanticTypeKind::Interface ||
        type.Kind() == SemanticTypeKind::NamedError) {
        const std::string name(type.Name());
        result += "_" + std::to_string(name.size()) + name;
    }
    if (type.Target()) result += "T" + MangleType(*type.Target());
    if (type.Kind() == SemanticTypeKind::Array)
        result += "L" + std::to_string(type.Length());
    if (type.Kind() == SemanticTypeKind::Tuple) {
        result += "E" + std::to_string(type.Elements().size());
        for (const SemanticType element : type.Elements())
            result += MangleType(element);
    }
    if (type.Kind() == SemanticTypeKind::Function) {
        result += "R" + MangleType(type.Signature().result);
        result += "P" + std::to_string(type.Signature().parameters.size());
        for (const SemanticType parameter : type.Signature().parameters)
            result += MangleType(parameter);
    }
    return result;
}

std::string OverloadLinkName(const FunctionSymbol& function) {
    std::string result = function.Name() + "$overload$";
    for (const SemanticType parameter : function.ParameterTypes())
        result += MangleType(parameter);
    return result;
}

bool SameParameterTypes(const FunctionSymbol& left,
                        const FunctionSymbol& right) {
    return left.ParameterTypes() == right.ParameterTypes();
}

std::optional<SemanticType> PrimitiveNumericType(const std::string& name) {
    if (name == "i8") return SemanticType(SemanticTypeKind::I8);
    if (name == "i16") return SemanticType(SemanticTypeKind::I16);
    if (name == "i32") return SemanticType(SemanticTypeKind::I32);
    if (name == "i64") return SemanticType(SemanticTypeKind::I64);
    if (name == "isize") return SemanticType(SemanticTypeKind::ISize);
    if (name == "u8") return SemanticType(SemanticTypeKind::U8);
    if (name == "u16") return SemanticType(SemanticTypeKind::U16);
    if (name == "u32") return SemanticType(SemanticTypeKind::U32);
    if (name == "u64") return SemanticType(SemanticTypeKind::U64);
    if (name == "usize") return SemanticType(SemanticTypeKind::USize);
    if (name == "f32") return SemanticType(SemanticTypeKind::F32);
    if (name == "f64") return SemanticType(SemanticTypeKind::F64);
    return std::nullopt;
}

bool IsFloatingConstant(std::string_view name) {
    return name == "nan" || name == "infinity" || name == "negative_infinity" ||
           name == "min_value" || name == "max_value" ||
           name == "min_positive" || name == "epsilon";
}

bool IsArrayElement(SemanticType type) {
    if (type.IsConst()) return false;
    if (type.Kind() == SemanticTypeKind::Array)
        return IsArrayElement(*type.Target());
    return type.IsNumeric() || type.IsErrorValue() ||
           type.Kind() == SemanticTypeKind::Enum ||
           type.Kind() == SemanticTypeKind::Structure ||
           type.Kind() == SemanticTypeKind::Bool ||
           type.Kind() == SemanticTypeKind::Char ||
           type.Kind() == SemanticTypeKind::Pointer ||
           type.Kind() == SemanticTypeKind::Function;
}

bool IsSliceElement(SemanticType type) {
    if (type.IsConst()) type = type.WithoutConst();
    return IsArrayElement(type) || type.Kind() == SemanticTypeKind::String ||
           type.Kind() == SemanticTypeKind::OwnedBuffer ||
           type.Kind() == SemanticTypeKind::OwnedSlice;
}

bool IsExternScalar(SemanticType type) {
    return type.IsNumeric() || type.Kind() == SemanticTypeKind::Bool ||
           type.Kind() == SemanticTypeKind::Char;
}

bool IsExternPointerTarget(SemanticType type) {
    if (IsExternScalar(type) || type.Kind() == SemanticTypeKind::Void)
        return true;
    if (type.Kind() == SemanticTypeKind::Pointer)
        return IsExternPointerTarget(*type.Target());
    return false;
}

bool IsExternAbiType(SemanticType type, bool allowVoid,
                     bool allowOpaqueHandle = false) {
    if (type.Kind() == SemanticTypeKind::Void) return allowVoid;
    if (type.Kind() == SemanticTypeKind::Pointer) {
        // These two opaque handles are never passed by value or nested. Their
        // layouts are exposed only behind a direct parameter pointer for
        // versioned runtime bridges.
        if (allowOpaqueHandle &&
            (type.Target()->Kind() == SemanticTypeKind::String ||
             type.Target()->Kind() == SemanticTypeKind::StringView))
            return true;
        return IsExternPointerTarget(*type.Target());
    }
    return IsExternScalar(type);
}

bool HasOwnershipContract(const core::TypeNode& type) {
    if (type.Contract() != core::OwnershipContract::None) return true;
    if (type.Target() && HasOwnershipContract(*type.Target())) return true;
    for (const auto& element : type.Elements())
        if (HasOwnershipContract(*element)) return true;
    return false;
}

bool HasBorrowedResult(SemanticType type) {
    std::unordered_set<const StructureType*> structures;
    const auto contains = [&](const auto& self,
                              SemanticType candidate) -> bool {
        if (candidate.HasBorrowedLifetime()) return true;
        if (candidate.Kind() == SemanticTypeKind::Tuple)
            return std::any_of(
                candidate.Elements().begin(), candidate.Elements().end(),
                [&](SemanticType element) { return self(self, element); });
        if (candidate.Kind() == SemanticTypeKind::Array)
            return self(self, *candidate.Target());
        if (candidate.Kind() == SemanticTypeKind::Structure) {
            if (!candidate.Structure()->validLayout) return false;
            if (!structures.insert(candidate.Structure()).second) return false;
            for (const StructureField& field : candidate.Structure()->fields)
                if (self(self, field.type)) return true;
        }
        return false;
    };
    return contains(contains, type);
}

bool RequiresBorrowSource(SemanticType type) {
    std::unordered_set<const StructureType*> structures;
    const auto contains = [&](const auto& self,
                              SemanticType candidate) -> bool {
        if (candidate.IsBorrowed()) return true;
        if (candidate.Kind() == SemanticTypeKind::Tuple)
            return std::any_of(
                candidate.Elements().begin(), candidate.Elements().end(),
                [&](SemanticType element) { return self(self, element); });
        if (candidate.Kind() == SemanticTypeKind::Array)
            return self(self, *candidate.Target());
        if (candidate.Kind() == SemanticTypeKind::Structure) {
            if (!candidate.Structure()->validLayout) return false;
            if (!structures.insert(candidate.Structure()).second) return false;
            for (const StructureField& field : candidate.Structure()->fields)
                if (self(self, field.type)) return true;
        }
        return false;
    };
    return contains(contains, type);
}

bool CanCarryBorrow(SemanticType type) {
    std::unordered_set<const StructureType*> structures;
    const auto carries = [&](const auto& self, SemanticType candidate) -> bool {
        if (candidate.HasBorrowedLifetime() ||
            candidate.Kind() == SemanticTypeKind::Function)
            return true;
        if (candidate.Kind() == SemanticTypeKind::Tuple)
            return std::any_of(
                candidate.Elements().begin(), candidate.Elements().end(),
                [&](SemanticType element) { return self(self, element); });
        if (candidate.Kind() == SemanticTypeKind::Array ||
            candidate.Kind() == SemanticTypeKind::OwnedBuffer ||
            candidate.Kind() == SemanticTypeKind::OwnedSlice)
            return self(self, *candidate.Target());
        if (candidate.Kind() == SemanticTypeKind::Structure) {
            if (!candidate.Structure()->validLayout) return false;
            if (!structures.insert(candidate.Structure()).second) return false;
            for (const StructureField& field : candidate.Structure()->fields)
                if (self(self, field.type)) return true;
        }
        return false;
    };
    return carries(carries, type);
}

bool ContainsRawPointer(SemanticType type) {
    std::unordered_set<const StructureType*> structures;
    std::unordered_set<const CallableSignature*> callables;
    const auto contains = [&](const auto& self,
                              SemanticType candidate) -> bool {
        if (candidate.Kind() == SemanticTypeKind::Pointer ||
            candidate.Kind() == SemanticTypeKind::OwnedBuffer)
            return true;
        if (candidate.Kind() == SemanticTypeKind::Tuple)
            return std::any_of(
                candidate.Elements().begin(), candidate.Elements().end(),
                [&](SemanticType element) { return self(self, element); });
        // A checked reference is not itself a raw-pointer capability. Its
        // target may encapsulate raw storage behind an ordinary safe API, and
        // borrowing that value must remain usable in safe callables (for
        // example, comparators over move-only collection elements).
        if (candidate.Kind() == SemanticTypeKind::Reference) return false;
        if (candidate.Kind() == SemanticTypeKind::Array ||
            candidate.Kind() == SemanticTypeKind::Slice)
            return self(self, *candidate.Target());
        if (candidate.Kind() == SemanticTypeKind::Structure) {
            if (!structures.insert(candidate.Structure()).second) return false;
            for (const auto& field : candidate.Structure()->fields)
                if (self(self, field.type)) return true;
        }
        if (candidate.Kind() == SemanticTypeKind::Function) {
            const auto* signature = &candidate.Signature();
            if (!callables.insert(signature).second) return false;
            if (self(self, signature->result)) return true;
            for (const auto& parameter : signature->parameters)
                if (self(self, parameter)) return true;
        }
        return false;
    };
    return contains(contains, type);
}

bool IsRawResource(SemanticType type) {
    return type.Kind() == SemanticTypeKind::Pointer ||
           type.Kind() == SemanticTypeKind::OwnedBuffer;
}

bool ContainsUnencapsulatedRawResource(SemanticType type) {
    std::unordered_set<const StructureType*> structures;
    const auto contains = [&](const auto& self,
                              SemanticType candidate) -> bool {
        if (IsRawResource(candidate)) return true;
        if (candidate.Kind() == SemanticTypeKind::Array)
            return self(self, *candidate.Target());
        if (candidate.Kind() == SemanticTypeKind::Tuple)
            return std::any_of(
                candidate.Elements().begin(), candidate.Elements().end(),
                [&](SemanticType element) { return self(self, element); });
        if (candidate.Kind() == SemanticTypeKind::Structure) {
            if (!structures.insert(candidate.Structure()).second) return false;
            const FunctionSymbol* tear = candidate.Structure()->tear;
            if (tear != nullptr && !tear->IsUnsafe()) return false;
            for (const StructureField& field : candidate.Structure()->fields)
                if (self(self, field.type)) return true;
        }
        return false;
    };
    return contains(contains, type);
}

std::vector<core::OwnershipContract> ResultContracts(
    const core::TypeNode& type) {
    std::vector<core::OwnershipContract> result;
    if (type.TypeForm() == core::TypeKind::Tuple) {
        for (const auto& element : type.Elements())
            result.push_back(element->Contract());
    } else if (type.TypeForm() != core::TypeKind::Primitive ||
               type.Type() != core::PrimitiveType::Void) {
        result.push_back(type.Contract());
    }
    return result;
}

// Keep byte and bit layout arithmetic representable by the native target and
// LLVM.
std::optional<std::uint64_t> StorageSize(SemanticType type,
                                         bool allowBorrowed = false) {
    constexpr std::uint64_t MAX_BYTES =
        std::numeric_limits<std::ptrdiff_t>::max() / 8;
    if (type.Kind() == SemanticTypeKind::Structure) {
        if (!type.Structure()->validLayout) return std::nullopt;
        std::uint64_t size = 0;
        for (const auto& field : type.Structure()->fields) {
            const auto bytes = StorageSize(field.type, true);
            if (!bytes || *bytes > MAX_BYTES - 16 ||
                size > MAX_BYTES - *bytes - 16)
                return std::nullopt;
            size += *bytes + 16;
        }
        return size;
    }
    if (type.Kind() == SemanticTypeKind::Tuple) {
        std::uint64_t size = 0;
        for (const auto& field : type.Elements()) {
            const auto bytes = StorageSize(field, allowBorrowed);
            // Conservative padding bound for the currently supported field
            // types.
            if (!bytes || *bytes > MAX_BYTES - 16 ||
                size > MAX_BYTES - *bytes - 16)
                return std::nullopt;
            size += *bytes + 16;
        }
        return size;
    }
    if (type.Kind() == SemanticTypeKind::Array) {
        const std::optional<std::uint64_t> element =
            StorageSize(*type.Target());
        if (!element || type.Length() > MAX_BYTES ||
            (*element != 0 && type.Length() > MAX_BYTES / *element))
            return std::nullopt;
        return type.Length() * *element;
    }
    switch (type.Kind()) {
        case SemanticTypeKind::Enum:
            return StorageSize(type.Enum()->backing);
        case SemanticTypeKind::NamedError:
            return 4;
        case SemanticTypeKind::ErrorValue:
            return 2 * sizeof(void*);
        case SemanticTypeKind::String:
            return 3 * sizeof(void*);
        case SemanticTypeKind::I8:
        case SemanticTypeKind::U8:
        case SemanticTypeKind::Bool:
        case SemanticTypeKind::Char:
            return 1;
        case SemanticTypeKind::I16:
        case SemanticTypeKind::U16:
            return 2;
        case SemanticTypeKind::I32:
        case SemanticTypeKind::U32:
        case SemanticTypeKind::F32:
            return 4;
        case SemanticTypeKind::I64:
        case SemanticTypeKind::U64:
        case SemanticTypeKind::F64:
            return 8;
        case SemanticTypeKind::ISize:
        case SemanticTypeKind::USize:
        case SemanticTypeKind::Pointer:
            return sizeof(void*);
        case SemanticTypeKind::OwnedBuffer:
        case SemanticTypeKind::OwnedSlice:
            return 2 * sizeof(void*);
        case SemanticTypeKind::Function:
            return 3 * sizeof(void*);
        case SemanticTypeKind::Slice:
            return allowBorrowed
                       ? std::optional<std::uint64_t>(2 * sizeof(void*))
                       : std::nullopt;
        case SemanticTypeKind::Reference:
            return allowBorrowed ? std::optional<std::uint64_t>(sizeof(void*))
                                 : std::nullopt;
        default:
            return std::nullopt;
    }
}

std::optional<std::uint64_t> ConstantIndex(const core::Expression& expression) {
    if (expression.Kind() == core::AstNodeKind::ParenthesizedExpression)
        return ConstantIndex(
            static_cast<const core::ParenthesizedExpression&>(expression)
                .Inner());
    if (expression.Kind() == core::AstNodeKind::LiteralExpression) {
        const auto& literal =
            static_cast<const core::LiteralExpression&>(expression);
        if (literal.LiteralType() != TokenType::NumberLiteral)
            return std::nullopt;
        std::uint64_t value = 0;
        const std::string& text = literal.Value();
        if (core::ParseUnsignedIntegerLiteral(text, value)) return value;
    }
    if (expression.Kind() == core::AstNodeKind::UnaryExpression) {
        const auto& unary =
            static_cast<const core::UnaryExpression&>(expression);
        if (unary.OperatorType() == TokenType::Add)
            return ConstantIndex(unary.Operand());
    }
    if (expression.Kind() == core::AstNodeKind::BinaryExpression) {
        const auto& binary =
            static_cast<const core::BinaryExpression&>(expression);
        const std::optional<std::uint64_t> left = ConstantIndex(binary.Left());
        const std::optional<std::uint64_t> right =
            ConstantIndex(binary.Right());
        if (!left || !right) return std::nullopt;
        constexpr std::uint64_t MAX =
            std::numeric_limits<std::uintptr_t>::max();
        switch (binary.OperatorType()) {
            case TokenType::Add:
                if (*left <= MAX - *right) return *left + *right;
                break;
            case TokenType::Sub:
                if (*left >= *right) return *left - *right;
                break;
            case TokenType::Mul:
                if (*right == 0 || *left <= MAX / *right) return *left * *right;
                break;
            case TokenType::Div:
                if (*right != 0) return *left / *right;
                break;
            case TokenType::Mod:
                if (*right != 0) return *left % *right;
                break;
            default:
                break;
        }
    }
    return std::nullopt;
}

bool IsNoneLiteral(const core::Expression& expression) {
    const core::Expression* unwrapped = &expression;
    while (unwrapped->Kind() == core::AstNodeKind::ParenthesizedExpression)
        unwrapped =
            &static_cast<const core::ParenthesizedExpression&>(*unwrapped)
                 .Inner();
    return unwrapped->Kind() == core::AstNodeKind::LiteralExpression &&
           static_cast<const core::LiteralExpression&>(*unwrapped)
                   .LiteralType() == TokenType::None;
}

void MarkDirectConstruction(
    std::unordered_set<const core::Expression*>& directConstructions,
    const core::Expression& expression, bool direct) {
    const core::Expression* current = &expression;
    while (true) {
        if (direct)
            directConstructions.insert(current);
        else
            directConstructions.erase(current);
        if (current->Kind() != core::AstNodeKind::ParenthesizedExpression)
            return;
        current = &static_cast<const core::ParenthesizedExpression&>(*current)
                       .Inner();
    }
}

bool IsEqualityComparable(SemanticType type) {
    if (type.IsNumeric() || type.IsErrorValue() ||
        type.Kind() == SemanticTypeKind::Enum ||
        type.Kind() == SemanticTypeKind::Bool ||
        type.Kind() == SemanticTypeKind::Char ||
        type.Kind() == SemanticTypeKind::Pointer ||
        type.Kind() == SemanticTypeKind::String ||
        type.Kind() == SemanticTypeKind::StringView)
        return true;
    if (type.Kind() == SemanticTypeKind::Array)
        return IsEqualityComparable(*type.Target());
    if (type.Kind() == SemanticTypeKind::Tuple) {
        for (const auto& element : type.Elements())
            if (!IsEqualityComparable(element)) return false;
        return true;
    }
    if (type.Kind() == SemanticTypeKind::Structure) {
        if (!type.Structure()->validLayout) return false;
        for (const auto& field : type.Structure()->fields)
            if (!IsEqualityComparable(field.type)) return false;
        return true;
    }
    return false;
}

bool CopyMayAllocate(
    SemanticType type,
    std::unordered_set<const StructureType*>& visitedStructures) {
    if (type.Kind() == SemanticTypeKind::String) return true;
    if (type.Kind() == SemanticTypeKind::Structure) {
        if (!visitedStructures.insert(type.Structure()).second) return false;
        for (const auto& field : type.Structure()->fields)
            if (CopyMayAllocate(field.type, visitedStructures)) return true;
    }
    if (type.Kind() == SemanticTypeKind::Array)
        return CopyMayAllocate(*type.Target(), visitedStructures);
    if (type.Kind() == SemanticTypeKind::Tuple)
        for (const auto& element : type.Elements())
            if (CopyMayAllocate(element, visitedStructures)) return true;
    return false;
}

bool CopyMayAllocate(SemanticType type) {
    std::unordered_set<const StructureType*> visitedStructures;
    return CopyMayAllocate(type, visitedStructures);
}

}  // namespace

void SemanticAnalyzer::Report(diagnostics::DiagnosticCode code,
                              source::SourceRange range, std::string message) {
    _diagnostics.Report(code, range, std::move(message));
}

Scope& SemanticAnalyzer::AddScope(const Scope* parent) {
    _model._scopes.push_back(std::make_unique<Scope>(parent));
    return *_model._scopes.back();
}

void SemanticAnalyzer::SetSource(const std::string& id) {
    _currentSource = id;
    if (const auto module = _sourceModules.find(id);
        module != _sourceModules.end())
        _currentModule = module->second;
    const auto found = _sourceScopes.find(id);
    if (found != _sourceScopes.end()) _currentScope = found->second;
}

bool SemanticAnalyzer::ReportInaccessibleImport(const std::string& name,
                                                source::SourceRange range) {
    const auto module = _inaccessibleImports.find(_currentSource);
    if (module == _inaccessibleImports.end()) return false;
    const auto found = module->second.find(name);
    if (found == module->second.end()) return false;
    const auto quiver = _quiverNames.find(found->second->quiverId);
    Report(diagnostics::DiagnosticCode::PrivateAccess, range,
           "'" + name +
               "' is quiver-visible and can only be accessed from modules "
               "in '" +
               (quiver == _quiverNames.end() ? found->second->quiverId
                                             : quiver->second) +
               "'");
    return true;
}

bool SemanticAnalyzer::ReportAmbiguousImport(const std::string& name,
                                             source::SourceRange range) {
    const auto source = _ambiguousImports.find(_currentSource);
    if (source == _ambiguousImports.end()) return false;
    const auto found = source->second.find(name);
    if (found == source->second.end()) return false;
    std::string message = "ambiguous imported name '" + name + "'; candidates:";
    for (const auto* declaration : found->second)
        message += " " + declaration->Range().start.file;
    message += "; use explicit import aliases";
    Report(diagnostics::DiagnosticCode::DuplicateDeclaration, range,
           std::move(message));
    return true;
}

std::string SemanticAnalyzer::TopName(const core::Declaration& declaration,
                                      const std::string& name) const {
    if (declaration.Kind() == core::AstNodeKind::FunctionDeclaration) {
        const auto& function =
            static_cast<const core::FunctionDeclaration&>(declaration);
        if (function.IsTestHarness()) return "main";
        if (name == "main" && !function.IsTest() &&
            _entryModules.contains(declaration.moduleId))
            return name;
    }
    return declaration.moduleId + name;
}
void SemanticAnalyzer::BindImports(const core::CompilationUnit& unit) {
    _inaccessibleImports.clear();
    _ambiguousImports.clear();
    for (const auto& module : unit.modules) {
        auto* scope = _sourceScopes.at(module.sourceId);
        std::map<std::string, std::vector<const core::Declaration*>> candidates;
        for (const auto& import : module.imports) {
            const bool privileged =
                module.test && module.testPair == import.resolvedModule;
            for (const auto& declaration : unit.Declarations()) {
                const bool sameQuiver =
                    declaration->quiverId == module.quiverId;
                const bool accessible =
                    declaration->exported ||
                    (declaration->quiverVisible && sameQuiver) || privileged;
                if (declaration->moduleId != import.resolvedModule) continue;
                std::string name;
                if (declaration->Kind() ==
                    core::AstNodeKind::FunctionDeclaration) {
                    const auto& function =
                        static_cast<const core::FunctionDeclaration&>(
                            *declaration);
                    if (!function.Owner().empty()) continue;
                    name = EffectName(function.Name(), function.IsCrashable());
                } else if (declaration->Kind() ==
                           core::AstNodeKind::StructureDeclaration)
                    name = static_cast<const core::StructureDeclaration&>(
                               *declaration)
                               .Name();
                else if (declaration->Kind() ==
                         core::AstNodeKind::InterfaceDeclaration)
                    name = static_cast<const core::InterfaceDeclaration&>(
                               *declaration)
                               .Name();
                else if (declaration->Kind() ==
                         core::AstNodeKind::EnumDeclaration)
                    name =
                        static_cast<const core::EnumDeclaration&>(*declaration)
                            .Name();
                else if (declaration->Kind() ==
                         core::AstNodeKind::ErrorDeclaration)
                    name =
                        static_cast<const core::ErrorDeclaration&>(*declaration)
                            .Name();
                else if (declaration->Kind() ==
                         core::AstNodeKind::CallableTypeDeclaration)
                    name = static_cast<const core::CallableTypeDeclaration&>(
                               *declaration)
                               .Name();
                else if (declaration->Kind() ==
                         core::AstNodeKind::ModuleConstantDeclaration)
                    name = static_cast<const core::ModuleConstantDeclaration&>(
                               *declaration)
                               .Name();
                if (!accessible) {
                    if (declaration->quiverVisible && !name.empty())
                        _inaccessibleImports[module.sourceId].insert_or_assign(
                            import.alias + "::" + name, declaration.get());
                    continue;
                }
                if (!name.empty()) {
                    auto& found = candidates[import.alias + "::" + name];
                    if (std::find(found.begin(), found.end(),
                                  declaration.get()) == found.end())
                        found.push_back(declaration.get());
                }
            }
        }
        for (const auto& [name, found] : candidates) {
            const bool functionOverloads =
                found.size() > 1 &&
                std::all_of(found.begin(), found.end(),
                            [](const core::Declaration* declaration) {
                                return declaration->Kind() ==
                                       core::AstNodeKind::FunctionDeclaration;
                            }) &&
                std::all_of(found.begin(), found.end(),
                            [&](const core::Declaration* declaration) {
                                return declaration->moduleId ==
                                       found.front()->moduleId;
                            });
            if (found.size() > 1 && !functionOverloads) {
                _ambiguousImports[module.sourceId].insert_or_assign(name,
                                                                    found);
                continue;
            }
            if (functionOverloads) {
                for (const core::Declaration* declaration : found) {
                    const Symbol* symbol = _model.DeclaredSymbol(*declaration);
                    if (symbol && symbol->Kind() == SymbolKind::Function)
                        (void)scope->DeclareOverloadAs(
                            name, *static_cast<const FunctionSymbol*>(symbol));
                }
            } else {
                const Symbol* symbol = _model.DeclaredSymbol(*found.front());
                if (symbol) (void)scope->DeclareAs(name, *symbol);
            }
        }
    }
}

void SemanticAnalyzer::ValidateDecorations(const core::CompilationUnit& unit) {
    using core::AstNodeKind;
    using core::Declaration;
    using core::DecorationApplication;
    using core::DecorationArgumentKind;
    using core::DecorationDeclaration;
    using core::StructureDeclaration;

    std::unordered_map<
        std::string,
        std::unordered_map<std::string, const DecorationDeclaration*>>
        declarations;
    for (const auto& declaration : unit.Declarations()) {
        if (declaration->Kind() != AstNodeKind::DecorationDeclaration) continue;
        const auto& decoration =
            static_cast<const DecorationDeclaration&>(*declaration);
        auto& names = declarations[declaration->moduleId];
        const auto [previous, inserted] =
            names.emplace(decoration.Name(), &decoration);
        if (!inserted)
            Report(
                diagnostics::DiagnosticCode::DuplicateDeclaration,
                decoration.NameRange(),
                "duplicate decoration declaration '" + decoration.Name() + "'");
    }

    const auto sourceModule =
        [&](const std::string& sourceId) -> const core::Module* {
        for (const auto& module : unit.modules)
            if (module.sourceId == sourceId) return &module;
        return nullptr;
    };
    const auto inModule =
        [&](const std::string& moduleId,
            const std::string& name) -> const DecorationDeclaration* {
        const auto module = declarations.find(moduleId);
        if (module == declarations.end()) return nullptr;
        const auto found = module->second.find(name);
        return found == module->second.end() ? nullptr : found->second;
    };
    const auto resolve =
        [&](const DecorationApplication& application,
            const Declaration& context) -> const DecorationDeclaration* {
        if (application.path.size() == 1) {
            if (const auto* local =
                    inModule(context.moduleId, application.Name()))
                return local;
        }
        const core::Module* module = sourceModule(context.sourceId);
        if (!module) return nullptr;
        const std::string alias = application.path.front();
        const std::string name = application.path.size() == 1
                                     ? application.path.front()
                                     : application.path.back();
        if (application.path.size() > 2) return nullptr;
        const DecorationDeclaration* result = nullptr;
        for (const auto& import : module->imports) {
            if (import.alias != alias) continue;
            const auto* candidate = inModule(import.resolvedModule, name);
            if (!candidate || !candidate->exported) continue;
            if (result && result != candidate) return nullptr;
            result = candidate;
        }
        return result;
    };
    const auto resolveOrReport =
        [&](const DecorationApplication& application,
            const Declaration& context) -> const DecorationDeclaration* {
        const auto* result = resolve(application, context);
        if (!result)
            Report(diagnostics::DiagnosticCode::InvalidDecoration,
                   application.range,
                   "unknown or inaccessible decoration '@" +
                       application.Name() + "'");
        return result;
    };
    const auto requireCompilerCounterpart =
        [&](const DecorationApplication& application,
            const DecorationDeclaration* decoration) {
            if (!decoration || decoration->Name() == "json") return;
            Report(diagnostics::DiagnosticCode::InvalidDecoration,
                   application.range,
                   "decoration '@" + decoration->Name() +
                       "' has no compiler implementation in Flecha 0.3");
        };

    for (const auto& declaration : unit.Declarations()) {
        std::unordered_set<const DecorationDeclaration*> seen;
        for (const auto& application : declaration->decorations) {
            const auto* decoration = resolveOrReport(application, *declaration);
            requireCompilerCounterpart(application, decoration);
            if (decoration && !seen.insert(decoration).second)
                Report(diagnostics::DiagnosticCode::InvalidDecoration,
                       application.range,
                       "duplicate decoration '@" + decoration->Name() +
                           "' on the same declaration");
            if (decoration && decoration->Name() == "json" &&
                declaration->Kind() != AstNodeKind::StructureDeclaration)
                Report(diagnostics::DiagnosticCode::InvalidDecoration,
                       application.range,
                       "@json can target only structures and their fields");
        }
        if (declaration->Kind() != AstNodeKind::StructureDeclaration) continue;

        const auto& structure =
            static_cast<const StructureDeclaration&>(*declaration);
        const DecorationApplication* structureJson = nullptr;
        const DecorationDeclaration* jsonDecoration = nullptr;
        const DecorationApplication* jsonApplication = nullptr;
        for (const auto& application : declaration->decorations) {
            const auto* decoration = resolve(application, *declaration);
            if (decoration && decoration->Name() == "json") {
                structureJson = &application;
                jsonDecoration = decoration;
                jsonApplication = &application;
            }
        }

        JsonDecorationMode mode = JsonDecorationMode::Explicit;
        bool hasJson = structureJson != nullptr;
        if (structureJson) {
            if (structureJson->arguments.size() == 1 &&
                structureJson->arguments.front().kind ==
                    DecorationArgumentKind::Identifier &&
                structureJson->arguments.front().value == "structure") {
                mode = JsonDecorationMode::Default;
            } else {
                Report(diagnostics::DiagnosticCode::InvalidDecoration,
                       structureJson->range,
                       "structure @json expects the 'structure' target");
            }
        }

        struct Candidate final {
            const core::StructureField* field;
            const DecorationApplication* application;
            std::string name;
        };
        std::vector<Candidate> candidates;
        for (const auto& field : structure.Fields()) {
            const DecorationApplication* fieldJson = nullptr;
            std::unordered_set<const DecorationDeclaration*> seen;
            for (const auto& application : field->Decorations()) {
                const auto* decoration =
                    resolveOrReport(application, *declaration);
                requireCompilerCounterpart(application, decoration);
                if (decoration && !seen.insert(decoration).second)
                    Report(diagnostics::DiagnosticCode::InvalidDecoration,
                           application.range,
                           "duplicate decoration '@" + decoration->Name() +
                               "' on field '" + field->Name() + "'");
                if (decoration && decoration->Name() == "json")
                    fieldJson = &application;
            }
            if (fieldJson) {
                hasJson = true;
                if (!jsonApplication) {
                    jsonApplication = fieldJson;
                    jsonDecoration = resolve(*fieldJson, *declaration);
                }
                if (field->IsPrivate())
                    Report(diagnostics::DiagnosticCode::InvalidDecoration,
                           fieldJson->range,
                           "private field '" + field->Name() +
                               "' cannot be serialized");
                if (fieldJson->arguments.size() != 1 ||
                    fieldJson->arguments.front().kind !=
                        DecorationArgumentKind::String)
                    Report(diagnostics::DiagnosticCode::InvalidDecoration,
                           fieldJson->range,
                           "field @json expects one string key");
            }
            const bool selected =
                !field->IsPrivate() &&
                (mode == JsonDecorationMode::Default || fieldJson != nullptr);
            if (!selected) continue;
            std::string name = field->Name();
            if (fieldJson && fieldJson->arguments.size() == 1 &&
                fieldJson->arguments.front().kind ==
                    DecorationArgumentKind::String)
                name = fieldJson->arguments.front().value;
            candidates.push_back({field.get(), fieldJson, std::move(name)});
        }
        if (!hasJson) continue;

        JsonStructureMetadata metadata{mode, {}};
        std::unordered_map<std::string, const Candidate*> names;
        for (const auto& candidate : candidates) {
            const auto [previous, inserted] =
                names.emplace(candidate.name, &candidate);
            if (!inserted) {
                const source::SourceRange range =
                    candidate.application ? candidate.application->range
                                          : candidate.field->NameRange();
                Report(diagnostics::DiagnosticCode::InvalidDecoration, range,
                       "duplicate JSON key '" + candidate.name + "'");
                continue;
            }
            metadata.fields.push_back({candidate.field, candidate.name});
        }
        if (jsonDecoration && jsonApplication) {
            metadata.writerTypeName =
                jsonDecoration->moduleId == structure.moduleId
                    ? "Writer"
                    : jsonApplication->path.front() + "::Writer";
            metadata.readerTypeName =
                jsonDecoration->moduleId == structure.moduleId
                    ? "Reader"
                    : jsonApplication->path.front() + "::Reader";
            SetSource(structure.sourceId);
            const Symbol* writer =
                _currentScope->Lookup(metadata.writerTypeName);
            if (!writer || writer->Kind() != SymbolKind::StructureType) {
                Report(diagnostics::DiagnosticCode::InvalidDecoration,
                       jsonApplication->range,
                       "@json requires its decoration module to export "
                       "structure Writer");
            } else {
                metadata.writer = writer->Type().Structure();
            }
            const Symbol* reader =
                _currentScope->Lookup(metadata.readerTypeName);
            if (reader && reader->Kind() != SymbolKind::StructureType) {
                Report(diagnostics::DiagnosticCode::InvalidDecoration,
                       jsonApplication->range,
                       "@json decoration module Reader must be a structure");
            } else if (reader) {
                metadata.reader = reader->Type().Structure();
            }
        }
        _model._jsonMetadata.emplace(&structure, std::move(metadata));
    }
}

void SemanticAnalyzer::ValidateExports(const core::CompilationUnit& unit) {
    // Template API visibility is checked before substitution. A caller may
    // instantiate a public generic method with its own private argument type.
    std::unordered_set<const Symbol*> specializedFunctions;
    for (const auto& declaration : unit.Declarations())
        if (_testModules.contains(declaration->moduleId) &&
            (declaration->exported || declaration->quiverVisible))
            Report(diagnostics::DiagnosticCode::PrivateAccess,
                   declaration->Range(),
                   "test modules cannot export or quiver declarations");
    for (const auto& declaration : unit.Declarations())
        if (declaration->Kind() == core::AstNodeKind::FunctionDeclaration &&
            !declaration->instantiationOrigins.empty())
            specializedFunctions.insert(_model.DeclaredSymbol(*declaration));
    std::unordered_map<const ErrorFamily*, const core::Declaration*> errors;
    std::unordered_map<const EnumType*, const core::Declaration*> enums;
    for (const auto& declaration : unit.Declarations())
        if (declaration->Kind() == core::AstNodeKind::ErrorDeclaration) {
            const auto* symbol = _model.DeclaredSymbol(*declaration);
            if (symbol)
                errors.emplace(&symbol->Type().Family(), declaration.get());
        }
    for (const auto& declaration : unit.Declarations())
        if (declaration->Kind() == core::AstNodeKind::EnumDeclaration) {
            const auto* symbol = _model.DeclaredSymbol(*declaration);
            if (symbol) enums.emplace(symbol->Type().Enum(), declaration.get());
        }
    const auto declarationVisible = [](const core::Declaration& api,
                                       const core::Declaration& type) {
        if (api.exported) return type.exported;
        if (api.quiverVisible)
            return type.exported ||
                   (type.quiverVisible && type.quiverId == api.quiverId);
        return true;
    };
    std::function<bool(SemanticType, const core::Declaration&)> visible =
        [&](SemanticType type, const core::Declaration& api) {
            if (type.Kind() == SemanticTypeKind::Structure)
                return declarationVisible(api, *type.Structure()->syntax);
            if (type.Kind() == SemanticTypeKind::Interface)
                return declarationVisible(api, *type.Interface()->syntax);
            if (type.Kind() == SemanticTypeKind::NamedError) {
                const auto found = errors.find(&type.Family());
                return found == errors.end() ||
                       declarationVisible(api, *found->second);
            }
            if (type.Kind() == SemanticTypeKind::Enum) {
                const auto found = enums.find(type.Enum());
                return found == enums.end() ||
                       declarationVisible(api, *found->second);
            }
            if (type.Kind() == SemanticTypeKind::Tuple) {
                for (const auto& element : type.Elements())
                    if (!visible(element, api)) return false;
            }
            if (type.Kind() == SemanticTypeKind::Function) {
                if (!visible(type.Signature().result, api)) return false;
                for (const auto& parameter : type.Signature().parameters)
                    if (!visible(parameter, api)) return false;
            }
            return !type.Target() || visible(*type.Target(), api);
        };
    const auto check = [&](SemanticType type, source::SourceRange range,
                           const core::Declaration& api) {
        if (!visible(type, api)) {
            const std::string message =
                api.exported
                    ? "exported APIs cannot expose non-exported types; export "
                      "the type or make the member private"
                    : "quiver APIs cannot expose module-private types; mark "
                      "the type quiver or export it, or make the member "
                      "private";
            Report(diagnostics::DiagnosticCode::PrivateAccess, range, message);
        }
    };
    for (const auto& interfaceType : _model._interfaces) {
        if (!interfaceType->syntax->exported &&
            !interfaceType->syntax->quiverVisible)
            continue;
        for (const auto& parent : interfaceType->parents)
            check(SemanticType(parent.type), parent.range,
                  *interfaceType->syntax);
    }
    for (const auto& structure : _model._structures) {
        if (!structure->syntax->exported && !structure->syntax->quiverVisible)
            continue;
        for (const auto& conformance : structure->conformances)
            check(SemanticType(conformance.type), conformance.range,
                  *structure->syntax);
        for (const auto& field : structure->fields)
            if (!field.syntax->IsPrivate())
                check(field.type, field.syntax->Range(), *structure->syntax);
        for (const auto& [name, methods] : structure->methods)
            for (const FunctionSymbol* method : methods) {
                if (method->IsPrivate() || name == "tear" ||
                    specializedFunctions.contains(method))
                    continue;
                check(method->ReturnType(), method->DeclarationRange(),
                      *structure->syntax);
                for (const auto& parameter : method->ParameterTypes())
                    check(parameter, method->DeclarationRange(),
                          *structure->syntax);
            }
    }
    for (const auto& enumeration : _model._enums) {
        if (!enumeration->syntax->exported &&
            !enumeration->syntax->quiverVisible)
            continue;
        for (const auto& conformance : enumeration->conformances)
            check(SemanticType(conformance.type), conformance.range,
                  *enumeration->syntax);
        for (const auto& [name, methods] : enumeration->methods)
            for (const FunctionSymbol* method : methods) {
                if (method->IsPrivate()) continue;
                check(method->ReturnType(), method->DeclarationRange(),
                      *enumeration->syntax);
                for (const auto& parameter : method->ParameterTypes())
                    check(parameter, method->DeclarationRange(),
                          *enumeration->syntax);
            }
    }
    for (const auto& declaration : unit.Declarations()) {
        if ((!declaration->exported && !declaration->quiverVisible) ||
            declaration->Kind() != core::AstNodeKind::FunctionDeclaration)
            continue;
        const auto* function = static_cast<const FunctionSymbol*>(
            _model.DeclaredSymbol(*declaration));
        if (!function) continue;
        check(function->ReturnType(), declaration->Range(), *declaration);
        for (const auto& parameter : function->ParameterTypes())
            check(parameter, declaration->Range(), *declaration);
    }
    for (const auto& declaration : unit.Declarations()) {
        if ((!declaration->exported && !declaration->quiverVisible) ||
            declaration->Kind() != core::AstNodeKind::CallableTypeDeclaration)
            continue;
        const auto* symbol = _model.DeclaredSymbol(*declaration);
        if (!symbol || symbol->Type().Kind() != SemanticTypeKind::Function)
            continue;
        const auto& signature = symbol->Type().Signature();
        check(signature.result, declaration->Range(), *declaration);
        for (const auto& parameter : signature.parameters)
            check(parameter, declaration->Range(), *declaration);
    }
    for (const auto& declaration : unit.Declarations()) {
        if ((!declaration->exported && !declaration->quiverVisible) ||
            declaration->Kind() != core::AstNodeKind::ModuleConstantDeclaration)
            continue;
        const auto* symbol = _model.DeclaredSymbol(*declaration);
        if (symbol) check(symbol->Type(), declaration->Range(), *declaration);
    }
}

const Symbol* SemanticAnalyzer::AddSymbol(std::unique_ptr<Symbol> symbol,
                                          const core::AstNode& declaration) {
    const Symbol* result = symbol.get();
    _model._symbols.push_back(std::move(symbol));
    _model._declarations.emplace(&declaration, result);
    std::string localName = result->Name();
    if (declaration.Kind() == core::AstNodeKind::StructureDeclaration)
        localName =
            static_cast<const core::StructureDeclaration&>(declaration).Name();
    else if (declaration.Kind() == core::AstNodeKind::InterfaceDeclaration)
        localName =
            static_cast<const core::InterfaceDeclaration&>(declaration).Name();
    else if (declaration.Kind() == core::AstNodeKind::EnumDeclaration)
        localName =
            static_cast<const core::EnumDeclaration&>(declaration).Name();
    else if (declaration.Kind() == core::AstNodeKind::ErrorDeclaration)
        localName =
            static_cast<const core::ErrorDeclaration&>(declaration).Name();
    else if (declaration.Kind() == core::AstNodeKind::CallableTypeDeclaration)
        localName =
            static_cast<const core::CallableTypeDeclaration&>(declaration)
                .Name();
    else if (declaration.Kind() == core::AstNodeKind::ModuleConstantDeclaration)
        localName =
            static_cast<const core::ModuleConstantDeclaration&>(declaration)
                .Name();
    else if (declaration.Kind() == core::AstNodeKind::FunctionDeclaration) {
        const auto& function =
            static_cast<const core::FunctionDeclaration&>(declaration);
        if (function.Owner().empty())
            localName = EffectName(function.Name(), function.IsCrashable());
    }
    Scope* declarationScope = _currentScope;
    const bool topLevel =
        declaration.Kind() == core::AstNodeKind::FunctionDeclaration ||
        declaration.Kind() == core::AstNodeKind::StructureDeclaration ||
        declaration.Kind() == core::AstNodeKind::InterfaceDeclaration ||
        declaration.Kind() == core::AstNodeKind::EnumDeclaration ||
        declaration.Kind() == core::AstNodeKind::ErrorDeclaration ||
        declaration.Kind() == core::AstNodeKind::CallableTypeDeclaration ||
        declaration.Kind() == core::AstNodeKind::ModuleConstantDeclaration;
    if (topLevel) {
        const auto& syntax = static_cast<const core::Declaration&>(declaration);
        if (const auto module = _moduleScopes.find(syntax.moduleId);
            module != _moduleScopes.end())
            declarationScope = module->second;
    }
    bool declared = false;
    const auto& existing = declarationScope->LookupAllLocal(localName);
    if (result->Kind() == SymbolKind::Function && !existing.empty() &&
        std::all_of(existing.begin(), existing.end(), [](const Symbol* item) {
            return item->Kind() == SymbolKind::Function;
        })) {
        auto* function =
            static_cast<FunctionSymbol*>(const_cast<Symbol*>(result));
        const auto functions = [&] {
            std::vector<const FunctionSymbol*> values;
            values.reserve(existing.size());
            for (const Symbol* item : existing)
                values.push_back(static_cast<const FunctionSymbol*>(item));
            return values;
        }();
        const bool foreign = function->IsExtern() ||
                             std::any_of(functions.begin(), functions.end(),
                                         [](const FunctionSymbol* candidate) {
                                             return candidate->IsExtern();
                                         });
        const bool mixedSafety = std::any_of(
            functions.begin(), functions.end(),
            [&](const FunctionSymbol* candidate) {
                return candidate->IsUnsafe() != function->IsUnsafe();
            });
        const bool duplicate =
            std::any_of(functions.begin(), functions.end(),
                        [&](const FunctionSymbol* candidate) {
                            return SameParameterTypes(*candidate, *function);
                        });
        if (foreign) {
            Report(diagnostics::DiagnosticCode::InvalidExtern,
                   result->DeclarationRange(),
                   "extern functions cannot be overloaded");
        } else if (mixedSafety) {
            Report(diagnostics::DiagnosticCode::DuplicateDeclaration,
                   result->DeclarationRange(),
                   "an overload set cannot mix safe and unsafe functions");
        } else if (duplicate) {
            Report(diagnostics::DiagnosticCode::DuplicateDeclaration,
                   result->DeclarationRange(),
                   "duplicate overload of '" + localName +
                       "' with the same parameter types");
        } else {
            declared =
                declarationScope->DeclareOverloadAs(localName, *function);
            function->SetLinkName(OverloadLinkName(*function));
            for (const FunctionSymbol* candidate : functions)
                const_cast<FunctionSymbol*>(candidate)->SetLinkName(
                    OverloadLinkName(*candidate));
        }
    } else {
        declared = declarationScope->DeclareAs(localName, *result);
    }
    if (declared && localName != result->Name()) {
        if (result->Kind() == SymbolKind::Function &&
            !declarationScope->LookupAllLocal(result->Name()).empty())
            (void)declarationScope->DeclareOverloadAs(
                result->Name(), *static_cast<const FunctionSymbol*>(result));
        else
            (void)declarationScope->Declare(*result);
    }
    if (!declared) {
        if (existing.empty() || result->Kind() != SymbolKind::Function ||
            std::any_of(existing.begin(), existing.end(),
                        [](const Symbol* item) {
                            return item->Kind() != SymbolKind::Function;
                        }))
            Report(diagnostics::DiagnosticCode::DuplicateDeclaration,
                   result->DeclarationRange(),
                   "duplicate declaration of '" + result->Name() + "'");
        return nullptr;
    }
    return result;
}

SemanticType SemanticAnalyzer::ResolveTypeSyntax(const core::TypeNode& type) {
    if (type.TypeForm() == core::TypeKind::Named) {
        if (ReportAmbiguousImport(type.Name(), type.Range()))
            return SemanticType::Error();
        const Symbol* symbol = _currentScope->Lookup(type.Name());
        if (symbol == nullptr || (symbol->Kind() != SymbolKind::ErrorType &&
                                  symbol->Kind() != SymbolKind::EnumType &&
                                  symbol->Kind() != SymbolKind::StructureType &&
                                  symbol->Kind() != SymbolKind::InterfaceType &&
                                  symbol->Kind() != SymbolKind::CallableType)) {
            if (!ReportInaccessibleImport(type.Name(), type.Range()))
                Report(diagnostics::DiagnosticCode::UnknownType, type.Range(),
                       "unknown type '" + type.Name() + "'");
            return SemanticType::Error();
        }
        const std::size_t textualLength =
            type.Name().size() + (type.IsConst() ? 6 : 0);
        // Compiler-generated type syntax reuses the enclosing source range.
        // Only index ranges that can represent the actual written type name.
        if (type.Range().Length() == textualLength)
            _model._typeBindings.emplace(&type, symbol);
        return type.IsConst() ? symbol->Type().WithConst() : symbol->Type();
    }
    if (type.TypeForm() == core::TypeKind::Primitive)
        return SemanticType::FromSyntax(type);
    if (type.TypeForm() == core::TypeKind::Tuple) {
        std::vector<SemanticType> elements;
        for (const auto& element : type.Elements())
            elements.push_back(ResolveTypeSyntax(*element));
        return SemanticType(std::move(elements), type.IsConst());
    }
    const bool ownedBuffer =
        type.TypeForm() == core::TypeKind::Pointer && type.Target() &&
        type.Target()->TypeForm() == core::TypeKind::Slice &&
        type.Target()->Contract() != core::OwnershipContract::Own;
    if (ownedBuffer)
        return SemanticType(SemanticTypeKind::OwnedBuffer,
                            ResolveTypeSyntax(*type.Target()->Target()),
                            type.IsConst());
    if (type.TypeForm() == core::TypeKind::Slice &&
        type.Contract() == core::OwnershipContract::Own)
        return SemanticType(SemanticTypeKind::OwnedSlice,
                            ResolveTypeSyntax(*type.Target()), type.IsConst());
    const SemanticTypeKind kind =
        type.TypeForm() == core::TypeKind::Pointer ? SemanticTypeKind::Pointer
        : type.TypeForm() == core::TypeKind::Reference
            ? SemanticTypeKind::Reference
        : type.TypeForm() == core::TypeKind::Array ? SemanticTypeKind::Array
                                                   : SemanticTypeKind::Slice;
    return SemanticType(kind, ResolveTypeSyntax(*type.Target()), type.IsConst(),
                        type.Length());
}

void SemanticAnalyzer::DeclareCallableTypes(const core::CompilationUnit& unit) {
    for (const auto& declaration : unit.Declarations()) {
        if (declaration->Kind() != core::AstNodeKind::CallableTypeDeclaration)
            continue;
        const auto& syntax =
            static_cast<const core::CallableTypeDeclaration&>(*declaration);
        SetSource(syntax.sourceId);
        auto signature = std::make_shared<CallableSignature>();
        _callableSignatures.emplace(&syntax, signature);
        AddSymbol(std::make_unique<Symbol>(
                      SymbolKind::CallableType, TopName(syntax, syntax.Name()),
                      SemanticType(signature), syntax.NameRange(), false),
                  syntax);
    }
}

void SemanticAnalyzer::ResolveCallableTypes(const core::CompilationUnit& unit) {
    for (const auto& declaration : unit.Declarations()) {
        if (declaration->Kind() != core::AstNodeKind::CallableTypeDeclaration)
            continue;
        const auto& syntax =
            static_cast<const core::CallableTypeDeclaration&>(*declaration);
        SetSource(syntax.sourceId);
        CallableSignature signature;
        signature.isUnsafe = syntax.IsUnsafe();
        signature.isOnce = syntax.IsOnce();
        signature.isCrashable = syntax.IsCrashable();
        std::unordered_set<std::string> parameterNames;
        for (const auto& parameter : syntax.Parameters()) {
            const SemanticType type = ResolveType(parameter->Type());
            signature.parameters.push_back(type);
            signature.parameterContracts.push_back(
                parameter->Type().Contract());
            if (!parameter->Name().empty() &&
                !parameterNames.insert(parameter->Name()).second)
                Report(
                    diagnostics::DiagnosticCode::DuplicateDeclaration,
                    parameter->NameRange(),
                    "duplicate callable parameter '" + parameter->Name() + "'");
            if (ContainsRawPointer(type) && !syntax.IsUnsafe())
                Report(diagnostics::DiagnosticCode::UnsafeOperation,
                       parameter->Type().Range(),
                       "raw pointer callable parameters require 'unsafe "
                       "func'");
        }

        std::vector<SemanticType> results;
        for (const auto& result : syntax.Results()) {
            const SemanticType type = ResolveType(*result, true);
            results.push_back(type);
            signature.resultContracts.push_back(result->Contract());
            if (type.Kind() == SemanticTypeKind::Void)
                Report(diagnostics::DiagnosticCode::InvalidReturnType,
                       result->Range(),
                       "an empty callable result list is spelled '=> ()'");
            if (ContainsRawPointer(type) && !syntax.IsUnsafe())
                Report(diagnostics::DiagnosticCode::UnsafeOperation,
                       result->Range(),
                       "raw pointer callable results require 'unsafe func'");
        }
        signature.result =
            results.empty()       ? SemanticType(SemanticTypeKind::Void)
            : results.size() == 1 ? results.front()
                                  : SemanticType(std::move(results));

        const bool requiresBorrowSource =
            RequiresBorrowSource(signature.result);
        if (requiresBorrowSource && !syntax.HasBorrowSources())
            Report(diagnostics::DiagnosticCode::InvalidBorrowContract,
                   syntax.Range(),
                   "reference and slice callable results require a '<- "
                   "source>' borrow contract");
        else if (!CanCarryBorrow(signature.result) && syntax.HasBorrowSources())
            Report(diagnostics::DiagnosticCode::InvalidBorrowContract,
                   syntax.BorrowSourceRanges().front(),
                   "a borrow contract requires a result that can carry a "
                   "borrow");
        else if (syntax.HasBorrowSources()) {
            std::set<std::size_t> seen;
            for (std::size_t sourceIndex = 0;
                 sourceIndex < syntax.BorrowSources().size(); ++sourceIndex) {
                std::optional<std::size_t> parameterIndex;
                for (std::size_t index = 0; index < syntax.Parameters().size();
                     ++index) {
                    if (syntax.Parameters()[index]->Name() ==
                        syntax.BorrowSources()[sourceIndex]) {
                        parameterIndex = index;
                        break;
                    }
                }

                const auto range = syntax.BorrowSourceRanges()[sourceIndex];
                if (!parameterIndex.has_value()) {
                    Report(diagnostics::DiagnosticCode::InvalidBorrowContract,
                           range,
                           "borrow source '" +
                               syntax.BorrowSources()[sourceIndex] +
                               "' is not a named callable parameter");
                } else if (!seen.insert(*parameterIndex).second) {
                    Report(diagnostics::DiagnosticCode::InvalidBorrowContract,
                           range,
                           "duplicate borrow source '" +
                               syntax.BorrowSources()[sourceIndex] + "'");
                } else if (!CanCarryBorrow(
                               signature.parameters[*parameterIndex])) {
                    Report(diagnostics::DiagnosticCode::InvalidBorrowContract,
                           range,
                           "a callable borrow source must be a parameter that "
                           "can carry a borrow");
                } else {
                    signature.borrowSourceIndices.push_back(*parameterIndex);
                }
            }
            std::sort(signature.borrowSourceIndices.begin(),
                      signature.borrowSourceIndices.end());
        }
        *_callableSignatures.at(&syntax) = std::move(signature);
    }
}

void SemanticAnalyzer::RegisterModuleConstants(
    const core::CompilationUnit& unit) {
    for (const auto& declaration : unit.Declarations()) {
        if (declaration->Kind() != core::AstNodeKind::ModuleConstantDeclaration)
            continue;
        const auto& constant =
            static_cast<const core::ModuleConstantDeclaration&>(*declaration);
        SetSource(constant.sourceId);
        const SemanticType type = ResolveType(constant.Type());
        const SemanticType valueType = type.WithoutConst();
        if (!(valueType.IsNumeric() ||
              valueType.Kind() == SemanticTypeKind::Bool ||
              valueType.Kind() == SemanticTypeKind::Char ||
              valueType.Kind() == SemanticTypeKind::StringView ||
              valueType.Kind() == SemanticTypeKind::Enum ||
              valueType.IsErrorValue()))
            Report(diagnostics::DiagnosticCode::InvalidVariableType,
                   constant.Type().Range(),
                   "module constants require a numeric, bool, char, stringv, "
                   "enum, or error-value type");
        const Symbol* symbol = AddSymbol(
            std::make_unique<VariableSymbol>(SymbolKind::ModuleConstant,
                                             TopName(constant, constant.Name()),
                                             type, constant.NameRange(), false),
            constant);
        if (symbol) _model._moduleConstants.emplace(symbol, &constant);
    }
}

bool SemanticAnalyzer::ValidateModuleConstantExpression(
    const core::Expression& expression,
    std::vector<const Symbol*>& dependencies) const {
    using core::AstNodeKind;
    switch (expression.Kind()) {
        case AstNodeKind::LiteralExpression:
            return true;
        case AstNodeKind::NameExpression: {
            const Symbol* symbol = _model.SymbolFor(
                static_cast<const core::NameExpression&>(expression));
            if (!symbol || symbol->Kind() != SymbolKind::ModuleConstant)
                return false;
            dependencies.push_back(symbol);
            return true;
        }
        case AstNodeKind::ParenthesizedExpression:
            return ValidateModuleConstantExpression(
                static_cast<const core::ParenthesizedExpression&>(expression)
                    .Inner(),
                dependencies);
        case AstNodeKind::UnaryExpression: {
            const auto& unary =
                static_cast<const core::UnaryExpression&>(expression);
            if (unary.OperatorType() != TokenType::Add &&
                unary.OperatorType() != TokenType::Sub &&
                unary.OperatorType() != TokenType::Bang &&
                unary.OperatorType() != TokenType::BitNot)
                return false;
            return ValidateModuleConstantExpression(unary.Operand(),
                                                    dependencies);
        }
        case AstNodeKind::BinaryExpression: {
            const auto& binary =
                static_cast<const core::BinaryExpression&>(expression);
            return ValidateModuleConstantExpression(binary.Left(),
                                                    dependencies) &&
                   ValidateModuleConstantExpression(binary.Right(),
                                                    dependencies);
        }
        case AstNodeKind::CastExpression:
            return ValidateModuleConstantExpression(
                static_cast<const core::CastExpression&>(expression).Value(),
                dependencies);
        case AstNodeKind::IsOfExpression:
            return ValidateModuleConstantExpression(
                static_cast<const core::IsOfExpression&>(expression).Value(),
                dependencies);
        case AstNodeKind::MemberAccessExpression: {
            const SemanticType type = _model.TypeOf(expression).WithoutConst();
            return _model.FieldFor(
                       static_cast<const core::MemberAccessExpression&>(
                           expression)) == nullptr &&
                   (type.IsNumeric() || type.Kind() == SemanticTypeKind::Enum ||
                    type.IsErrorValue());
        }
        default:
            return false;
    }
}

void SemanticAnalyzer::AnalyzeModuleConstants(
    const core::CompilationUnit& unit) {
    std::unordered_map<const Symbol*, std::vector<const Symbol*>> dependencies;
    for (const auto& declaration : unit.Declarations()) {
        if (declaration->Kind() != core::AstNodeKind::ModuleConstantDeclaration)
            continue;
        const auto& constant =
            static_cast<const core::ModuleConstantDeclaration&>(*declaration);
        const Symbol* symbol = _model.DeclaredSymbol(constant);
        if (!symbol) continue;
        SetSource(constant.sourceId);
        const SemanticType initializer =
            AnalyzeExpression(constant.Initializer(), symbol->Type());
        if (!initializer.IsError() &&
            !IsAssignable(symbol->Type(), initializer))
            Report(diagnostics::DiagnosticCode::IncompatibleType,
                   constant.Initializer().Range(),
                   TypeMismatch(symbol->Type(), initializer));
        std::vector<const Symbol*> found;
        if (!ValidateModuleConstantExpression(constant.Initializer(), found))
            Report(diagnostics::DiagnosticCode::InvalidOperand,
                   constant.Initializer().Range(),
                   "module constant initializer must be a compile-time "
                   "expression");
        dependencies.emplace(symbol, std::move(found));
    }

    std::unordered_map<const Symbol*, int> state;
    std::unordered_set<const Symbol*> reported;
    std::function<void(const Symbol*)> visit = [&](const Symbol* symbol) {
        state[symbol] = 1;
        for (const Symbol* dependency : dependencies[symbol]) {
            if (!dependencies.contains(dependency)) continue;
            if (state[dependency] == 0) {
                visit(dependency);
                continue;
            }
            if (state[dependency] != 1 || !reported.insert(symbol).second)
                continue;
            const auto* constant = _model.ModuleConstantFor(*symbol);
            Report(diagnostics::DiagnosticCode::InvalidOperand,
                   constant ? constant->Initializer().Range()
                            : symbol->DeclarationRange(),
                   "cyclic module constant definition involving '" +
                       symbol->Name() + "'");
        }
        state[symbol] = 2;
    };
    for (const auto& [symbol, ignored] : dependencies) {
        (void)ignored;
        if (state[symbol] == 0) visit(symbol);
    }
}

SemanticType SemanticAnalyzer::ResolveType(const core::TypeNode& type,
                                           bool allowBorrowedTuple) {
    const core::TypeNode* leaf = &type;
    while (leaf->Target() != nullptr) leaf = leaf->Target();
    if (leaf->TypeForm() == core::TypeKind::Tuple)
        for (const auto& element : leaf->Elements())
            (void)ResolveType(*element, allowBorrowedTuple);
    const SemanticType result = ResolveTypeSyntax(type);
    const SemanticType* layer = &result;
    if (layer->Kind() == SemanticTypeKind::Slice ||
        layer->Kind() == SemanticTypeKind::OwnedBuffer ||
        layer->Kind() == SemanticTypeKind::OwnedSlice) {
        if (!IsSliceElement(layer->Target()->WithoutConst()))
            Report(diagnostics::DiagnosticCode::UnsupportedCompoundType,
                   type.Range(),
                   "slice elements must have sized non-borrowed storage");
        layer = layer->Target();
    } else if (layer->Kind() == SemanticTypeKind::Reference) {
        layer = layer->Target();
        if (layer->Kind() == SemanticTypeKind::Void ||
            layer->Kind() == SemanticTypeKind::String ||
            layer->Kind() == SemanticTypeKind::StringView) {
            Report(diagnostics::DiagnosticCode::UnsupportedCompoundType,
                   type.Range(),
                   "references currently require scalar, array, structure, or "
                   "raw-pointer storage");
        }
    }
    while (layer->Target() != nullptr) {
        if (layer->IsBorrowed()) {
            Report(diagnostics::DiagnosticCode::UnsupportedCompoundType,
                   type.Range(),
                   "nested reference or slice storage is not yet supported: '" +
                       result.Name() + "'");
            break;
        }
        if (layer->Kind() == SemanticTypeKind::Array)
            ValidateArrayType(*layer, type.Range());
        layer = layer->Target();
    }
    if (layer->Kind() == SemanticTypeKind::Tuple)
        ValidateTupleType(*layer, type.Range(), allowBorrowedTuple);
    if (result.Kind() == SemanticTypeKind::Reference &&
        result.Target()->Kind() == SemanticTypeKind::Tuple &&
        result.Target()->OwnsStorage())
        Report(diagnostics::DiagnosticCode::UnsupportedCompoundType,
               type.Range(),
               "references to owning tuple storage are not yet supported");
    return result;
}

void SemanticAnalyzer::ValidateTupleType(SemanticType type,
                                         source::SourceRange range,
                                         bool allowBorrowed) {
    if (!StorageSize(type, allowBorrowed).has_value())
        Report(diagnostics::DiagnosticCode::InvalidTuple, range,
               "tuple has unsupported elements or its storage is too large");
    for (const SemanticType& element : type.Elements()) {
        if (element.Kind() == SemanticTypeKind::Tuple)
            ValidateTupleType(element, range, allowBorrowed);
        else if (element.Kind() == SemanticTypeKind::Array)
            ValidateArrayType(element, range);
        else if (!element.IsError() && !element.IsErrorValue() &&
                 !element.IsNumeric() &&
                 element.Kind() != SemanticTypeKind::Enum &&
                 element.Kind() != SemanticTypeKind::Structure &&
                 element.Kind() != SemanticTypeKind::Bool &&
                 element.Kind() != SemanticTypeKind::Char &&
                 element.Kind() != SemanticTypeKind::Pointer &&
                 element.Kind() != SemanticTypeKind::OwnedBuffer &&
                 element.Kind() != SemanticTypeKind::OwnedSlice &&
                 element.Kind() != SemanticTypeKind::String &&
                 element.Kind() != SemanticTypeKind::Function &&
                 !(allowBorrowed && element.HasBorrowedLifetime()))
            Report(diagnostics::DiagnosticCode::InvalidTuple, range,
                   "tuple elements must be owned values: scalars, enums, error "
                   "values, raw "
                   "pointers, strings, structures, fixed arrays, or tuples; "
                   "found '" +
                       element.Name() + "'");
    }
}

SemanticType SemanticAnalyzer::AnalyzeTuple(
    const core::TupleExpression& expression, SemanticType expected) {
    std::vector<SemanticType> elements;
    const bool contextual =
        expected.Kind() == SemanticTypeKind::Tuple &&
        expected.Elements().size() == expression.Elements().size();
    for (std::size_t i = 0; i < expression.Elements().size(); ++i) {
        const SemanticType target =
            contextual ? expected.Elements()[i] : SemanticType::Error();
        const SemanticType actual =
            AnalyzeExpression(*expression.Elements()[i], target);
        if (!target.IsError() && !IsAssignable(target, actual))
            Report(diagnostics::DiagnosticCode::IncompatibleType,
                   expression.Elements()[i]->Range(),
                   TypeMismatch(target, actual));
        elements.push_back(target.IsError() ? actual.WithoutConst() : target);
    }
    SemanticType result(std::move(elements));
    ValidateTupleType(result, expression.Range(),
                      contextual && HasBorrowedResult(expected));
    return result;
}

void SemanticAnalyzer::AnalyzeDestructure(
    const core::DestructureStatement& statement) {
    std::vector<SemanticType> expected;
    std::vector<const Symbol*> targets;
    std::unordered_set<std::string> names;
    bool introducesBinding = false;
    for (const auto& binding : statement.Bindings()) {
        const Symbol* target = nullptr;
        SemanticType type = SemanticType::Error();
        if (binding->Name() != "_" && !names.insert(binding->Name()).second)
            Report(diagnostics::DiagnosticCode::InvalidDestructure,
                   binding->Range(),
                   "duplicate destructuring destination '" + binding->Name() +
                       "'");
        if (binding->ExplicitType() != nullptr) {
            type = ResolveType(*binding->ExplicitType());
            if (statement.IsInferred())
                Report(diagnostics::DiagnosticCode::InvalidDestructure,
                       binding->Range(),
                       "typed destructuring bindings require '=' rather than "
                       "':='");
        } else if (binding->Name() != "_") {
            target = statement.IsInferred()
                         ? _currentScope->LookupLocal(binding->Name())
                         : _currentScope->Lookup(binding->Name());
            if (statement.IsInferred() && target == nullptr)
                introducesBinding = true;
            if (target == nullptr) {
                if (!statement.IsInferred()) {
                    Report(diagnostics::DiagnosticCode::UnknownName,
                           binding->Range(),
                           "unknown name '" + binding->Name() + "'");
                }
            } else {
                type = target->Type();
                if (target->Kind() == SymbolKind::Function ||
                    !target->Writable())
                    Report(diagnostics::DiagnosticCode::SymbolNotWritable,
                           binding->Range(),
                           "destructuring destination is not writable");
            }
        }
        expected.push_back(type);
        targets.push_back(target);
    }
    if (statement.IsInferred() && !introducesBinding)
        Report(diagnostics::DiagnosticCode::InvalidDestructure,
               statement.Range(),
               "short destructuring declaration requires at least one new "
               "variable");
    const SemanticType value =
        AnalyzeExpression(statement.Value(), SemanticType(expected));
    if (value.Kind() != SemanticTypeKind::Tuple) {
        if (!value.IsError())
            Report(diagnostics::DiagnosticCode::InvalidDestructure,
                   statement.Value().Range(),
                   "destructuring requires a tuple value");
        return;
    }
    if (value.IsImmovable())
        Report(diagnostics::DiagnosticCode::NonCopyableValue,
               statement.Value().Range(),
               "an immovable tuple cannot be destructured");
    else
        CheckTransfer(statement.Value(), value);
    const core::Expression* transferred = &statement.Value();
    while (transferred->Kind() == core::AstNodeKind::ParenthesizedExpression)
        transferred =
            &static_cast<const core::ParenthesizedExpression&>(*transferred)
                 .Inner();
    if (transferred->Kind() == core::AstNodeKind::TupleExpression) {
        const auto& tuple =
            static_cast<const core::TupleExpression&>(*transferred);
        for (std::size_t index = 0;
             index < tuple.Elements().size() && index < targets.size();
             ++index) {
            const core::Expression* element = tuple.Elements()[index].get();
            while (element->Kind() ==
                   core::AstNodeKind::ParenthesizedExpression)
                element =
                    &static_cast<const core::ParenthesizedExpression&>(*element)
                         .Inner();
            if (targets[index] != nullptr &&
                element->Kind() == core::AstNodeKind::NameExpression &&
                _model.TransferOf(*element) == TransferKind::Move &&
                _model.SymbolFor(static_cast<const core::NameExpression&>(
                    *element)) == targets[index])
                Report(
                    diagnostics::DiagnosticCode::NonCopyableValue,
                    element->Range(),
                    "cannot move '" + targets[index]->Name() + "' into itself");
        }
    }
    if (statement.Bindings().size() != value.Elements().size() ||
        statement.Bindings().size() < 2) {
        Report(diagnostics::DiagnosticCode::InvalidDestructure,
               statement.Range(), "destructuring arity must match the tuple");
        return;
    }
    for (std::size_t i = 0; i < statement.Bindings().size(); ++i) {
        const auto& binding = *statement.Bindings()[i];
        const SemanticType actual = value.Elements()[i];
        const SemanticType type =
            expected[i].IsError() ? actual.WithoutConst() : expected[i];
        if (!IsAssignable(type, actual))
            Report(diagnostics::DiagnosticCode::IncompatibleType,
                   binding.Range(), TypeMismatch(type, actual));
        if (binding.Name() == "_") continue;
        const Symbol* target = targets[i];
        if ((statement.IsInferred() && target == nullptr) ||
            binding.ExplicitType() != nullptr)
            target = AddSymbol(std::make_unique<VariableSymbol>(
                                   SymbolKind::LocalVariable, binding.Name(),
                                   type, binding.Range(), !type.IsConst()),
                               binding);
        if (target != nullptr)
            _model._destructureTargets.emplace(&binding, target);
    }
}

void SemanticAnalyzer::RegisterBuiltIns() {
    const auto registerOutput = [this](std::string name,
                                       BuiltInFunction builtIn) {
        std::unique_ptr<FunctionSymbol> function =
            std::make_unique<FunctionSymbol>(
                std::move(name), SemanticType(SemanticTypeKind::Void),
                std::vector<SemanticType>{
                    SemanticType(SemanticTypeKind::StringView)},
                source::SourceRange{}, builtIn);
        const FunctionSymbol* symbol = function.get();
        _model._symbols.push_back(std::move(function));
        (void)_currentScope->Declare(*symbol);
    };
    registerOutput("stdout", BuiltInFunction::Stdout);
    registerOutput("stdoutln", BuiltInFunction::StdoutLine);
    registerOutput("stdoutf", BuiltInFunction::StdoutFormat);
    registerOutput("stderr", BuiltInFunction::Stderr);
    registerOutput("stderrln", BuiltInFunction::StderrLine);
    registerOutput("stderrf", BuiltInFunction::StderrFormat);
    {
        auto function = std::make_unique<FunctionSymbol>(
            "crash", SemanticType(SemanticTypeKind::Void),
            std::vector<SemanticType>{
                SemanticType(SemanticTypeKind::StringView)},
            source::SourceRange{}, BuiltInFunction::Crash);
        (void)_currentScope->Declare(*function);
        _model._symbols.push_back(std::move(function));
    }
    for (const auto& [name, builtIn] :
         {std::pair{"stdin", BuiltInFunction::Stdin},
          std::pair{"stdinln", BuiltInFunction::StdinLine}}) {
        auto function = std::make_unique<FunctionSymbol>(
            name, SemanticType(SemanticTypeKind::ErrorValue),
            std::vector<SemanticType>{
                SemanticType(SemanticTypeKind::StringView)},
            source::SourceRange{}, builtIn);
        (void)_currentScope->Declare(*function);
        _model._symbols.push_back(std::move(function));
    }
    {
        auto function = std::make_unique<FunctionSymbol>(
            "__sprintf",
            SemanticType(std::vector<SemanticType>{
                SemanticType(SemanticTypeKind::String),
                SemanticType(SemanticTypeKind::ErrorValue)}),
            std::vector<SemanticType>{
                SemanticType(SemanticTypeKind::StringView)},
            source::SourceRange{}, BuiltInFunction::StringFormat);
        (void)_currentScope->Declare(*function);
        _model._symbols.push_back(std::move(function));
    }
    {
        auto signature = std::make_shared<CallableSignature>();
        signature->result = SemanticType(SemanticTypeKind::String);
        signature->parameters = {SemanticType(SemanticTypeKind::StringView)};
        signature->isCrashable = true;
        auto function = std::make_unique<FunctionSymbol>(
            "__sprintf_crash", std::move(signature), source::SourceRange{},
            BuiltInFunction::StringFormatCrash);
        (void)_currentScope->DeclareAs("__sprintf_crash!", *function);
        _model._symbols.push_back(std::move(function));
    }
    for (const auto& [name, intrinsic] :
         {std::pair{"new", BuiltInFunction::New},
          std::pair{"free", BuiltInFunction::Free},
          std::pair{"__hash", BuiltInFunction::Hash},
          std::pair{"__storage_new", BuiltInFunction::StorageNew},
          std::pair{"__storage_initialize", BuiltInFunction::StorageInitialize},
          std::pair{"__storage_relocate", BuiltInFunction::StorageRelocate},
          std::pair{"__storage_extract", BuiltInFunction::StorageExtract},
          std::pair{"__storage_destroy", BuiltInFunction::StorageDestroy},
          std::pair{"__storage_free", BuiltInFunction::StorageFree}}) {
        auto function = std::make_unique<FunctionSymbol>(
            name, SemanticType(SemanticTypeKind::Void),
            std::vector<SemanticType>{}, source::SourceRange{}, intrinsic,
            true);
        (void)_currentScope->Declare(*function);
        _model._symbols.push_back(std::move(function));
    }
    {
        auto function = std::make_unique<FunctionSymbol>(
            "__json_slice_append", SemanticType(SemanticTypeKind::Void),
            std::vector<SemanticType>{}, source::SourceRange{},
            BuiltInFunction::JsonSliceAppend);
        (void)_currentScope->Declare(*function);
        _model._symbols.push_back(std::move(function));
    }
    {
        auto signature = std::make_shared<CallableSignature>();
        signature->result = SemanticType(SemanticTypeKind::Void);
        signature->isUnsafe = true;
        signature->isCrashable = true;
        auto function = std::make_unique<FunctionSymbol>(
            "new", std::move(signature), source::SourceRange{},
            BuiltInFunction::New);
        (void)_currentScope->DeclareAs("new!", *function);
        _model._symbols.push_back(std::move(function));
    }
    auto family = std::make_shared<ErrorFamily>();
    family->name = "AllocationError";
    family->displayName = family->name;
    family->variants = {"OutOfMemory"};
    auto error = std::make_unique<Symbol>(SymbolKind::ErrorType, family->name,
                                          SemanticType(family),
                                          source::SourceRange{}, false);
    (void)_currentScope->Declare(*error);
    _model._symbols.push_back(std::move(error));

    family = std::make_shared<ErrorFamily>();
    family->name = "InputError";
    family->displayName = family->name;
    family->variants = {"EndOfFile", "ReadFailed", "InvalidSyntax",
                        "OutOfRange", "PromptFailed"};
    error = std::make_unique<Symbol>(SymbolKind::ErrorType, family->name,
                                     SemanticType(family),
                                     source::SourceRange{}, false);
    (void)_currentScope->Declare(*error);
    _model._symbols.push_back(std::move(error));
}

void SemanticAnalyzer::RegisterErrorTypes(const core::CompilationUnit& unit) {
    for (const auto& declaration : unit.Declarations()) {
        if (declaration->Kind() != core::AstNodeKind::ErrorDeclaration)
            continue;
        const auto& error =
            static_cast<const core::ErrorDeclaration&>(*declaration);
        SetSource(error.sourceId);
        auto family = std::make_shared<ErrorFamily>();
        family->name = TopName(error, error.Name());
        family->displayName = error.Name();
        std::unordered_set<std::string> variants;
        for (const auto& variant : error.Variants()) {
            if (!variants.insert(variant.name).second)
                Report(diagnostics::DiagnosticCode::DuplicateDeclaration,
                       variant.range,
                       "duplicate error variant '" + variant.name + "'");
            family->variants.push_back(variant.name);
        }
        if (family->variants.empty())
            Report(diagnostics::DiagnosticCode::InvalidErrorDeclaration,
                   error.Range(),
                   "error types require at least one failure variant; none is "
                   "implicit");
        AddSymbol(std::make_unique<Symbol>(SymbolKind::ErrorType,
                                           TopName(error, error.Name()),
                                           SemanticType(std::move(family)),
                                           error.NameRange(), false),
                  error);
    }
}

void SemanticAnalyzer::RegisterInterfaces(const core::CompilationUnit& unit) {
    for (const auto& declaration : unit.Declarations()) {
        if (declaration->Kind() != core::AstNodeKind::InterfaceDeclaration)
            continue;
        const auto& syntax =
            static_cast<const core::InterfaceDeclaration&>(*declaration);
        SetSource(syntax.sourceId);
        auto interfaceType = std::make_unique<InterfaceType>();
        interfaceType->name = TopName(syntax, syntax.Name());
        interfaceType->displayName = syntax.specializationName.empty()
                                         ? syntax.Name()
                                         : syntax.specializationName;
        interfaceType->syntax = &syntax;
        AddSymbol(std::make_unique<InterfaceSymbol>(
                      TopName(syntax, syntax.Name()), interfaceType.get(),
                      syntax.NameRange()),
                  syntax);
        _model._interfaces.push_back(std::move(interfaceType));
    }
}

void SemanticAnalyzer::RegisterEnums(const core::CompilationUnit& unit) {
    const auto width = [](SemanticType type) -> unsigned {
        switch (type.Kind()) {
            case SemanticTypeKind::I8:
            case SemanticTypeKind::U8:
                return 8;
            case SemanticTypeKind::I16:
            case SemanticTypeKind::U16:
                return 16;
            case SemanticTypeKind::I32:
            case SemanticTypeKind::U32:
                return 32;
            case SemanticTypeKind::ISize:
            case SemanticTypeKind::USize:
                return sizeof(void*) * 8;
            default:
                return 64;
        }
    };
    const auto isSigned = [](SemanticType type) {
        return type.Kind() >= SemanticTypeKind::I8 &&
               type.Kind() <= SemanticTypeKind::ISize;
    };

    for (const auto& declaration : unit.Declarations()) {
        if (declaration->Kind() != core::AstNodeKind::EnumDeclaration) continue;
        const auto& syntax =
            static_cast<const core::EnumDeclaration&>(*declaration);
        SetSource(syntax.sourceId);

        auto enumeration = std::make_unique<EnumType>();
        enumeration->name = TopName(syntax, syntax.Name());
        enumeration->displayName = syntax.specializationName.empty()
                                       ? syntax.Name()
                                       : syntax.specializationName;
        enumeration->syntax = &syntax;
        if (syntax.BackingType() != nullptr) {
            enumeration->backing = ResolveType(*syntax.BackingType());
            if (!enumeration->backing.IsInteger() ||
                enumeration->backing.IsConst()) {
                Report(diagnostics::DiagnosticCode::InvalidEnumDeclaration,
                       syntax.BackingType()->Range(),
                       "enum backing types must be mutable integer types");
                enumeration->backing = SemanticType(SemanticTypeKind::U32);
            }
        }

        const unsigned bits = width(enumeration->backing);
        const bool signedBacking = isSigned(enumeration->backing);
        const std::uint64_t unsignedMaximum =
            bits == 64 ? std::numeric_limits<std::uint64_t>::max()
                       : (std::uint64_t{1} << bits) - 1;
        const std::uint64_t signedMaximum =
            (std::uint64_t{1} << (bits - 1)) - 1;
        const std::uint64_t signedMinimumMagnitude = signedMaximum + 1;
        const std::uint64_t mask = unsignedMaximum;

        std::unordered_set<std::string> names;
        std::unordered_set<std::uint64_t> values;
        bool nextAvailable = true;
        std::int64_t nextSigned = 0;
        std::uint64_t nextUnsigned = 0;
        bool hasDefault = false;
        for (const core::EnumVariant& variant : syntax.Variants()) {
            if (!names.insert(variant.name).second)
                Report(diagnostics::DiagnosticCode::DuplicateDeclaration,
                       variant.range,
                       "duplicate enum variant '" + variant.name + "'");

            std::uint64_t raw = 0;
            bool valid = true;
            if (variant.discriminant.empty()) {
                if (!nextAvailable) {
                    Report(diagnostics::DiagnosticCode::InvalidEnumDeclaration,
                           variant.range,
                           "implicit enum discriminant exceeds backing type '" +
                               enumeration->backing.Name() + "'");
                    valid = false;
                } else if (signedBacking) {
                    raw = static_cast<std::uint64_t>(nextSigned) & mask;
                } else {
                    raw = nextUnsigned;
                }
            } else {
                const bool negative = variant.discriminant.front() == '-';
                const std::string_view digits =
                    negative ? std::string_view(variant.discriminant).substr(1)
                             : std::string_view(variant.discriminant);
                std::uint64_t magnitude = 0;
                valid = core::ParseUnsignedIntegerLiteral(digits, magnitude);
                if (!signedBacking && negative) {
                    valid = false;
                } else if (signedBacking && negative) {
                    if (magnitude > signedMinimumMagnitude)
                        valid = false;
                    else
                        raw = (std::uint64_t{0} - magnitude) & mask;
                } else {
                    const std::uint64_t maximum =
                        signedBacking ? signedMaximum : unsignedMaximum;
                    if (magnitude > maximum)
                        valid = false;
                    else
                        raw = magnitude;
                }
                if (!valid)
                    Report(diagnostics::DiagnosticCode::InvalidEnumDeclaration,
                           variant.discriminantRange,
                           "enum discriminant is not representable as '" +
                               enumeration->backing.Name() + "'");
            }

            if (valid && !values.insert(raw).second)
                Report(diagnostics::DiagnosticCode::InvalidEnumDeclaration,
                       variant.range,
                       "duplicate enum discriminant value for '" +
                           variant.name + "'");
            enumeration->variants.push_back({variant.name, raw, variant.range});

            if (variant.isDefault) {
                if (hasDefault)
                    Report(diagnostics::DiagnosticCode::InvalidEnumDeclaration,
                           variant.range,
                           "an enum may declare exactly one default variant");
                else {
                    enumeration->defaultValue = raw;
                    hasDefault = true;
                }
            }

            if (!valid) {
                nextAvailable = false;
            } else if (signedBacking) {
                std::int64_t current = 0;
                if (bits == 64 && raw == (std::uint64_t{1} << 63))
                    current = std::numeric_limits<std::int64_t>::min();
                else if ((raw & (std::uint64_t{1} << (bits - 1))) != 0)
                    current = static_cast<std::int64_t>(raw | ~mask);
                else
                    current = static_cast<std::int64_t>(raw);
                if (current == (bits == 64
                                    ? std::numeric_limits<std::int64_t>::max()
                                    : static_cast<std::int64_t>(signedMaximum)))
                    nextAvailable = false;
                else
                    nextSigned = current + 1;
            } else if (raw == unsignedMaximum) {
                nextAvailable = false;
            } else {
                nextUnsigned = raw + 1;
            }
        }
        if (enumeration->variants.empty())
            Report(diagnostics::DiagnosticCode::InvalidEnumDeclaration,
                   syntax.Range(), "enum types require at least one variant");

        EnumType* type = enumeration.get();
        _model._enums.push_back(std::move(enumeration));
        AddSymbol(std::make_unique<Symbol>(
                      SymbolKind::EnumType, TopName(syntax, syntax.Name()),
                      SemanticType(type), syntax.NameRange(), false),
                  syntax);
    }
}

void SemanticAnalyzer::RegisterFunctions(const core::CompilationUnit& unit) {
    for (const auto& declaration : unit.Declarations())
        if (declaration->Kind() == core::AstNodeKind::FunctionDeclaration)
            RegisterFunction(
                static_cast<const core::FunctionDeclaration&>(*declaration));
}

void SemanticAnalyzer::RegisterFunction(
    const core::FunctionDeclaration& function, bool generated) {
    SetSource(function.sourceId);
    if (function.HasVariadicPack()) {
        if (!function.genericParameters.empty() ||
            !function.ownerParameters.empty())
            Report(diagnostics::DiagnosticCode::InvalidVariadic,
                   function.VariadicPackRange(),
                   "variadic forwarding functions cannot be generic");
        if (function.IsExtern())
            Report(diagnostics::DiagnosticCode::InvalidVariadic,
                   function.VariadicPackRange(),
                   "extern functions cannot declare a forwarding pack");
        for (const auto& parameter : function.Parameters()) {
            if (parameter->Name() == function.VariadicPackName())
                Report(diagnostics::DiagnosticCode::DuplicateDeclaration,
                       function.VariadicPackRange(),
                       "variadic pack conflicts with parameter '" +
                           parameter->Name() + "'");
            if (parameter->DefaultValue() != nullptr)
                Report(diagnostics::DiagnosticCode::InvalidVariadic,
                       parameter->DefaultValue()->Range(),
                       "variadic forwarding functions cannot have default "
                       "arguments");
        }
    }
    StructureType* owner = nullptr;
    EnumType* enumOwner = nullptr;
    InterfaceType* interfaceOwner = nullptr;
    if (!function.Owner().empty()) {
        const Symbol* type = _currentScope->Lookup(function.Owner());
        if (!type || (type->Kind() != SymbolKind::StructureType &&
                      type->Kind() != SymbolKind::EnumType &&
                      type->Kind() != SymbolKind::InterfaceType)) {
            Report(diagnostics::DiagnosticCode::UnknownType,
                   function.NameRange(),
                   "unknown structure, enum, or interface '" +
                       function.Owner() + "'");
        } else if (type->Kind() == SymbolKind::StructureType)
            owner = const_cast<StructureType*>(type->Type().Structure());
        else if (type->Kind() == SymbolKind::EnumType)
            enumOwner = const_cast<EnumType*>(type->Type().Enum());
        else
            interfaceOwner =
                const_cast<InterfaceType*>(type->Type().Interface());
    }
    std::vector<SemanticType> parameterTypes;
    std::vector<core::OwnershipContract> parameterContracts;
    bool receiver = false;
    bool sawDefault = false;
    for (const auto& parameter : function.Parameters()) {
        SemanticType type = ResolveType(parameter->Type());
        if (parameter->DefaultValue() != nullptr) {
            sawDefault = true;
            if (parameter->Name() == "self")
                Report(diagnostics::DiagnosticCode::InvalidDefaultArgument,
                       parameter->DefaultValue()->Range(),
                       "a method receiver cannot have a default argument");
            if (function.Name().starts_with("$anon"))
                Report(diagnostics::DiagnosticCode::InvalidDefaultArgument,
                       parameter->DefaultValue()->Range(),
                       "anonymous function parameters cannot have default "
                       "arguments");
        } else if (sawDefault) {
            Report(diagnostics::DiagnosticCode::InvalidDefaultArgument,
                   parameter->NameRange(),
                   "a required parameter cannot follow a defaulted "
                   "parameter");
        }
        if (parameter->Name() == "self") {
            const SemanticType receiverTarget =
                type.Kind() == SemanticTypeKind::Reference ||
                        type.Kind() == SemanticTypeKind::Pointer
                    ? *type.Target()
                    : type;
            const bool matchingOwner =
                (owner &&
                 receiverTarget.Kind() == SemanticTypeKind::Structure &&
                 receiverTarget.Structure() == owner) ||
                (enumOwner && receiverTarget.Kind() == SemanticTypeKind::Enum &&
                 receiverTarget.Enum() == enumOwner) ||
                (interfaceOwner &&
                 receiverTarget.Kind() == SemanticTypeKind::Interface &&
                 receiverTarget.Interface() == interfaceOwner);
            receiver = (owner || enumOwner || interfaceOwner) &&
                       parameterTypes.empty() &&
                       (type.Kind() == SemanticTypeKind::Structure ||
                        type.Kind() == SemanticTypeKind::Enum ||
                        type.Kind() == SemanticTypeKind::Interface ||
                        type.Kind() == SemanticTypeKind::Reference ||
                        type.Kind() == SemanticTypeKind::Pointer) &&
                       matchingOwner;
            if (!receiver)
                Report(diagnostics::DiagnosticCode::InvalidMember,
                       parameter->Range(),
                       "self must be the first parameter and own, borrow, or "
                       "point to the owning structure, enum, or interface");
        }
        parameterTypes.push_back(type);
        parameterContracts.push_back(parameter->Type().Contract());
    }
    const SemanticType returnType = ResolveType(function.ReturnType(), true);
    if (function.IsTest()) {
        bool valid =
            _testModules.contains(function.moduleId) && !function.exported &&
            function.Owner().empty() && function.genericParameters.empty() &&
            function.Parameters().size() == 1 && parameterTypes.size() == 1 &&
            parameterTypes[0].Kind() == SemanticTypeKind::Reference &&
            !parameterTypes[0].Target()->IsConst();
        if (valid) {
            const SemanticType context = *parameterTypes[0].Target();
            valid = context.Kind() == SemanticTypeKind::Structure &&
                    context.Structure()->syntax->moduleId == _testingModule &&
                    context.Structure()->syntax->Name() == "UnitTest";
        }
        if (!valid)
            Report(diagnostics::DiagnosticCode::InvalidMember, function.Range(),
                   "a test must be declared in a '<name>_test' module and take "
                   "exactly one mutable '&testing::UnitTest' parameter");
    }
    const bool requiresBorrowSource = RequiresBorrowSource(returnType);
    std::vector<std::size_t> borrowSourceIndices;
    if (returnType.Kind() == SemanticTypeKind::Tuple &&
        std::any_of(returnType.Elements().begin(), returnType.Elements().end(),
                    [](SemanticType element) {
                        return element.Kind() == SemanticTypeKind::Tuple &&
                               HasBorrowedResult(element);
                    }))
        Report(diagnostics::DiagnosticCode::InvalidBorrowContract,
               function.ReturnType().Range(),
               "borrowed multi-results must use a flat tuple");
    if (requiresBorrowSource && !function.HasBorrowSources()) {
        Report(diagnostics::DiagnosticCode::InvalidBorrowContract,
               function.ReturnType().Range(),
               "reference and slice results require a '<- source>' "
               "borrow contract");
    } else if (!CanCarryBorrow(returnType) && function.HasBorrowSources()) {
        Report(diagnostics::DiagnosticCode::InvalidBorrowContract,
               function.BorrowSourceRanges().empty()
                   ? function.VariadicPackRange()
                   : function.BorrowSourceRanges().front(),
               "a borrow contract requires a result that can carry a borrow");
    } else if (function.HasBorrowSources()) {
        std::set<std::size_t> seen;
        for (std::size_t sourceIndex = 0;
             sourceIndex < function.BorrowSources().size(); ++sourceIndex) {
            std::optional<std::size_t> parameterIndex;
            for (std::size_t index = 0; index < function.Parameters().size();
                 ++index) {
                if (function.Parameters()[index]->Name() ==
                    function.BorrowSources()[sourceIndex]) {
                    parameterIndex = index;
                    break;
                }
            }

            const auto range = function.BorrowSourceRanges()[sourceIndex];
            if (!parameterIndex.has_value()) {
                Report(
                    diagnostics::DiagnosticCode::InvalidBorrowContract, range,
                    "borrow source '" + function.BorrowSources()[sourceIndex] +
                        "' is not a parameter");
            } else if (!seen.insert(*parameterIndex).second) {
                Report(diagnostics::DiagnosticCode::InvalidBorrowContract,
                       range,
                       "duplicate borrow source '" +
                           function.BorrowSources()[sourceIndex] + "'");
            } else if (!CanCarryBorrow(parameterTypes[*parameterIndex])) {
                Report(
                    diagnostics::DiagnosticCode::InvalidBorrowContract, range,
                    "borrow source '" + function.BorrowSources()[sourceIndex] +
                        "' must be a parameter that can carry a borrow");
            } else {
                borrowSourceIndices.push_back(*parameterIndex);
            }
        }
        std::sort(borrowSourceIndices.begin(), borrowSourceIndices.end());
    }
    if (function.IsExtern()) {
        if (!function.Owner().empty() ||
            function.HasModifier(core::FunctionModifier::Private) ||
            function.HasModifier(core::FunctionModifier::Override))
            Report(diagnostics::DiagnosticCode::InvalidExtern, function.Range(),
                   "extern declarations must be free functions and cannot "
                   "be private or override lifecycle methods");
        if (!function.genericParameters.empty() ||
            !function.ownerParameters.empty())
            Report(diagnostics::DiagnosticCode::InvalidExtern, function.Range(),
                   "extern declarations cannot have generic parameters");
        if (function.Name() == "main")
            Report(diagnostics::DiagnosticCode::InvalidExtern,
                   function.NameRange(),
                   "the Flecha entry point cannot be an extern declaration");
        if (!IsExternAbiType(returnType, true) ||
            HasOwnershipContract(function.ReturnType()))
            Report(diagnostics::DiagnosticCode::InvalidExtern,
                   function.ReturnType().Range(),
                   "extern results must be ABI scalars or raw pointers "
                   "without ownership contracts");
        bool hasPointer = returnType.Kind() == SemanticTypeKind::Pointer;
        for (std::size_t index = 0; index < parameterTypes.size(); ++index) {
            const auto type = parameterTypes[index];
            hasPointer |= type.Kind() == SemanticTypeKind::Pointer;
            if (!IsExternAbiType(type, false, true) ||
                HasOwnershipContract(function.Parameters()[index]->Type()))
                Report(diagnostics::DiagnosticCode::InvalidExtern,
                       function.Parameters()[index]->Type().Range(),
                       "extern parameters must be ABI scalars or raw "
                       "pointers without ownership contracts");
            if (type.Kind() == SemanticTypeKind::Pointer &&
                (type.Target()->Kind() == SemanticTypeKind::String ||
                 type.Target()->Kind() == SemanticTypeKind::StringView) &&
                !function.Name().starts_with("flecha_runtime_v1_"))
                Report(diagnostics::DiagnosticCode::InvalidExtern,
                       function.Parameters()[index]->Type().Range(),
                       "opaque string handles are reserved for versioned "
                       "Flecha runtime symbols");
            if (type.Kind() == SemanticTypeKind::Pointer &&
                type.Target()->Kind() == SemanticTypeKind::String &&
                type.Target()->IsConst())
                Report(diagnostics::DiagnosticCode::InvalidExtern,
                       function.Parameters()[index]->Type().Range(),
                       "an opaque string pointer is an output handle and "
                       "must be mutable");
            if (type.Kind() == SemanticTypeKind::Pointer &&
                type.Target()->Kind() == SemanticTypeKind::StringView &&
                !type.Target()->IsConst())
                Report(diagnostics::DiagnosticCode::InvalidExtern,
                       function.Parameters()[index]->Type().Range(),
                       "an opaque stringv pointer is an input handle and "
                       "must point to const data");
        }
        if (hasPointer && !function.IsUnsafe())
            Report(diagnostics::DiagnosticCode::InvalidExtern,
                   function.NameRange(),
                   "extern declarations using raw pointers must be unsafe");
    }
    CallableSignature callableSignature{
        returnType,
        parameterTypes,
        parameterContracts,
        ResultContracts(function.ReturnType()),
        borrowSourceIndices,
        function.IsUnsafe(),
        function.HasModifier(core::FunctionModifier::Once),
        function.IsCrashable()};
    const MethodVisibility methodVisibility =
        function.HasModifier(core::FunctionModifier::Protected)
            ? MethodVisibility::Protected
            : MethodVisibility::Public;
    const MethodContract candidate = NormalizeMethodContract(
        function.Name(), callableSignature, receiver,
        function.HasVariadicPack(), function.BorrowsVariadicPack(),
        methodVisibility);
    MethodContract* interfaceDeclaration = nullptr;
    const MethodContract* inheritedContract = nullptr;
    if (interfaceOwner) {
        for (MethodContract& contract : interfaceOwner->declaredMethods)
            if (contract.key == candidate.key) {
                interfaceDeclaration = &contract;
                inheritedContract = &contract;
                break;
            }
    } else {
        const auto findContract = [&](const auto& conformances) {
            const MethodContract* result = nullptr;
            for (const auto& conformance : conformances)
                for (const MethodContract* contract : conformance.type->methods)
                    if (contract->key == candidate.key && result == nullptr)
                        result = contract;
            return result;
        };
        if (owner)
            inheritedContract = findContract(owner->conformances);
        else if (enumOwner)
            inheritedContract = findContract(enumOwner->conformances);
    }

    if (function.HasModifier(core::FunctionModifier::Private) &&
        function.HasModifier(core::FunctionModifier::Protected))
        Report(diagnostics::DiagnosticCode::InvalidMember, function.NameRange(),
               "a method cannot be both private and protected");
    if (function.HasModifier(core::FunctionModifier::Protected) &&
        inheritedContract == nullptr)
        Report(diagnostics::DiagnosticCode::InvalidInterface,
               function.NameRange(),
               "protected concrete methods must continue a protected "
               "interface method");
    if (interfaceOwner && interfaceDeclaration == nullptr)
        Report(diagnostics::DiagnosticCode::InvalidInterface,
               function.NameRange(),
               "interface method body has no matching declaration on '" +
                   interfaceOwner->displayName + "'");
    if (inheritedContract != nullptr) {
        if (!candidate.SameContract(*inheritedContract) ||
            function.HasModifier(core::FunctionModifier::Private))
            Report(diagnostics::DiagnosticCode::InvalidInterface,
                   function.NameRange(),
                   "method '" + function.Name() +
                       "' does not match the complete interface contract");
        if (!function.genericParameters.empty())
            Report(diagnostics::DiagnosticCode::InvalidInterface,
                   function.NameRange(),
                   "an interface method implementation cannot introduce "
                   "generic parameters");
        if (!interfaceOwner &&
            inheritedContract->category == core::MethodCategory::Final)
            Report(diagnostics::DiagnosticCode::InvalidInterface,
                   function.NameRange(),
                   "final interface method '" + function.Name() +
                       "' cannot be overridden");
        if (interfaceOwner &&
            interfaceDeclaration->category == core::MethodCategory::Virtual)
            Report(diagnostics::DiagnosticCode::InvalidInterface,
                   function.NameRange(),
                   "virtual interface method '" + function.Name() +
                       "' cannot have a body");
    }

    const bool lifecycle =
        !function.IsCrashable() && owner &&
        (function.Name() == "init" || function.Name() == "zero" ||
         function.Name() == "tear");
    const JsonStructureMetadata* jsonMetadata =
        owner ? _model.JsonMetadataFor(*owner->syntax) : nullptr;
    const bool jsonWriter = !function.IsCrashable() && jsonMetadata &&
                            function.Name() == "writeJson";
    const bool jsonFacade =
        !function.IsCrashable() && jsonMetadata && function.Name() == "json";
    const bool jsonReader = !function.IsCrashable() && jsonMetadata &&
                            jsonMetadata->reader &&
                            function.Name() == "readJson";
    const bool jsonFactory = !function.IsCrashable() && jsonMetadata &&
                             jsonMetadata->reader &&
                             function.Name() == "fromJson";
    const bool compilerOverride =
        (lifecycle || jsonWriter || jsonFacade || jsonReader || jsonFactory) &&
        !generated;
    const bool interfaceOverride =
        inheritedContract != nullptr &&
        ((!interfaceOwner) ||
         (interfaceOwner && interfaceDeclaration->inherited != nullptr));
    if (function.HasModifier(core::FunctionModifier::Override) !=
        (compilerOverride || interfaceOverride))
        Report(inheritedContract || interfaceOwner
                   ? diagnostics::DiagnosticCode::InvalidInterface
                   : diagnostics::DiagnosticCode::InvalidLifecycle,
               function.NameRange(),
               inheritedContract || interfaceOwner
                   ? "override is required exactly when implementing or "
                     "replacing an inherited interface method"
                   : "override is required exactly when replacing a "
                     "compiler-generated method");
    if (function.HasModifier(core::FunctionModifier::Private) && !owner &&
        !enumOwner && !interfaceOwner)
        Report(diagnostics::DiagnosticCode::InvalidMember, function.NameRange(),
               "private requires a structure or enum method");
    if (lifecycle && function.Name() == "init" &&
        (receiver || returnType.WithoutConst() != SemanticType(owner)))
        Report(diagnostics::DiagnosticCode::InvalidLifecycle, function.Range(),
               "init must be static and return its structure type");
    if (lifecycle && function.Name() == "zero" &&
        (function.IsUnsafe() || receiver || !parameterTypes.empty() ||
         returnType.WithoutConst() != SemanticType(owner)))
        Report(diagnostics::DiagnosticCode::InvalidLifecycle, function.Range(),
               "zero must be safe, static, take no parameters, and return its "
               "structure type");
    if (lifecycle && function.Name() == "zero" &&
        function.HasModifier(core::FunctionModifier::Private))
        Report(diagnostics::DiagnosticCode::InvalidLifecycle, function.Range(),
               "zero cannot be private because structure default "
               "construction is part of the type's API");
    if (lifecycle && function.Name() == "tear" &&
        (returnType.Kind() != SemanticTypeKind::Void ||
         parameterTypes.size() != 1 || !receiver ||
         parameterTypes[0].Kind() != SemanticTypeKind::Reference ||
         parameterTypes[0].Target()->IsConst()))
        Report(diagnostics::DiagnosticCode::InvalidLifecycle, function.Range(),
               "tear must return void and take only &self");
    if (jsonWriter) {
        const bool validReceiver =
            receiver && parameterTypes.size() == 2 &&
            parameterTypes[0].Kind() == SemanticTypeKind::Reference &&
            parameterTypes[0].Target()->IsConst();
        const bool validWriter =
            parameterTypes.size() == 2 && jsonMetadata->writer &&
            parameterTypes[1].Kind() == SemanticTypeKind::Reference &&
            !parameterTypes[1].Target()->IsConst() &&
            parameterTypes[1].Target()->Kind() == SemanticTypeKind::Structure &&
            parameterTypes[1].Target()->Structure() == jsonMetadata->writer;
        if (function.IsUnsafe() ||
            function.HasModifier(core::FunctionModifier::Private) ||
            returnType.Kind() != SemanticTypeKind::ErrorValue ||
            !validReceiver || !validWriter)
            Report(diagnostics::DiagnosticCode::InvalidDecoration,
                   function.Range(),
                   "writeJson override must be safe, public, return error, "
                   "and take &const self and the mutable @json Writer");
    }
    if (jsonFacade) {
        const bool validReceiver =
            receiver && parameterTypes.size() == 1 &&
            parameterTypes[0].Kind() == SemanticTypeKind::Reference &&
            parameterTypes[0].Target()->IsConst();
        const bool validResult =
            returnType.Kind() == SemanticTypeKind::Tuple &&
            returnType.Elements().size() == 2 &&
            returnType.Elements()[0].Kind() == SemanticTypeKind::OwnedSlice &&
            returnType.Elements()[0].Target()->Kind() == SemanticTypeKind::U8 &&
            returnType.Elements()[1].Kind() == SemanticTypeKind::ErrorValue;
        if (function.IsUnsafe() ||
            function.HasModifier(core::FunctionModifier::Private) ||
            !validReceiver || !validResult)
            Report(diagnostics::DiagnosticCode::InvalidDecoration,
                   function.Range(),
                   "json override must be safe, public, take &const self, "
                   "and return (own []u8, error)");
    }
    if (jsonReader) {
        const bool validReceiver =
            receiver && parameterTypes.size() == 2 &&
            parameterTypes[0].Kind() == SemanticTypeKind::Reference &&
            !parameterTypes[0].Target()->IsConst();
        const bool validReader =
            parameterTypes.size() == 2 && jsonMetadata->reader &&
            parameterTypes[1].Kind() == SemanticTypeKind::Reference &&
            !parameterTypes[1].Target()->IsConst() &&
            parameterTypes[1].Target()->Kind() == SemanticTypeKind::Structure &&
            parameterTypes[1].Target()->Structure() == jsonMetadata->reader;
        if (function.IsUnsafe() ||
            function.HasModifier(core::FunctionModifier::Private) ||
            returnType.Kind() != SemanticTypeKind::ErrorValue ||
            !validReceiver || !validReader)
            Report(diagnostics::DiagnosticCode::InvalidDecoration,
                   function.Range(),
                   "readJson override must be safe, public, return error, "
                   "and take &self and the mutable @json Reader");
    }
    if (jsonFactory) {
        const bool validParameter =
            !receiver && parameterTypes.size() == 1 &&
            parameterTypes[0].Kind() == SemanticTypeKind::Slice &&
            parameterTypes[0].Target()->Kind() == SemanticTypeKind::U8 &&
            parameterTypes[0].Target()->IsConst();
        const bool validResult =
            returnType.Kind() == SemanticTypeKind::Tuple &&
            returnType.Elements().size() == 2 &&
            returnType.Elements()[0].WithoutConst() == SemanticType(owner) &&
            returnType.Elements()[1].Kind() == SemanticTypeKind::ErrorValue;
        if (function.IsUnsafe() ||
            function.HasModifier(core::FunctionModifier::Private) ||
            !validParameter || !validResult)
            Report(
                diagnostics::DiagnosticCode::InvalidDecoration,
                function.Range(),
                "fromJson override must be safe, public, receiver-less, take "
                "[]const u8, and return its structure type with error");
    }
    if (enumOwner && !function.IsCrashable() &&
        (function.Name() == "init" || function.Name() == "zero" ||
         function.Name() == "tear"))
        Report(diagnostics::DiagnosticCode::InvalidLifecycle, function.Range(),
               "enum lifecycle is compiler-defined; init, zero, and tear are "
               "reserved");
    if ((owner || enumOwner || interfaceOwner) && function.exported)
        Report(diagnostics::DiagnosticCode::InvalidMember, function.Range(),
               "export the owning type, not individual member functions");
    if ((owner || enumOwner || interfaceOwner) && function.quiverVisible)
        Report(diagnostics::DiagnosticCode::InvalidMember, function.Range(),
               "member functions cannot be marked quiver; visibility comes "
               "from the owning type");
    if (owner && owner->syntax->moduleId != function.moduleId)
        Report(diagnostics::DiagnosticCode::PrivateAccess, function.Range(),
               "methods must be defined in their structure's module");
    if (enumOwner && enumOwner->syntax->moduleId != function.moduleId)
        Report(diagnostics::DiagnosticCode::PrivateAccess, function.Range(),
               "methods must be defined in their enum's module");
    if (interfaceOwner && interfaceOwner->syntax->moduleId != function.moduleId)
        Report(diagnostics::DiagnosticCode::PrivateAccess, function.Range(),
               "methods must be defined in their interface's module");
    std::string name = owner       ? owner->name + "::" + function.Name()
                       : enumOwner ? enumOwner->name + "::" + function.Name()
                       : interfaceOwner
                           ? interfaceOwner->name + "::" + function.Name()
                           : TopName(function, function.Name());
    if (function.IsCrashable() && name != "main") name += "!";
    auto signature =
        std::make_shared<const CallableSignature>(callableSignature);
    const auto* symbol = static_cast<const FunctionSymbol*>(
        AddSymbol(std::make_unique<FunctionSymbol>(
                      name, std::move(signature), function.NameRange(),
                      BuiltInFunction::None, owner,
                      function.HasModifier(core::FunctionModifier::Private),
                      receiver, function.IsExtern(),
                      function.IsExtern() ? function.Name() : "", enumOwner,
                      &function, function.BorrowsVariadicPack(), interfaceOwner,
                      function.HasModifier(core::FunctionModifier::Protected)),
                  function));
    if (symbol && function.HasModifier(core::FunctionModifier::Extern))
        _forbiddenZeroEffects.insert(symbol);
    if (owner && symbol) {
        const std::string methodName =
            EffectName(function.Name(), function.IsCrashable());
        owner->methods[methodName].push_back(symbol);
        if (lifecycle && function.Name() == "init") {
            owner->init = symbol;
            owner->hasCustomInit = !generated;
        }
        if (lifecycle && function.Name() == "zero") {
            owner->zero = symbol;
            owner->zeroIsGenerated = generated;
        }
        if (lifecycle && function.Name() == "tear") owner->tear = symbol;
    }
    if (enumOwner && symbol) {
        const std::string methodName =
            EffectName(function.Name(), function.IsCrashable());
        enumOwner->methods[methodName].push_back(symbol);
        for (const EnumVariant& variant : enumOwner->variants)
            if (variant.name == function.Name())
                Report(diagnostics::DiagnosticCode::DuplicateDeclaration,
                       function.NameRange(),
                       "method conflicts with enum variant '" + variant.name +
                           "'");
    }
    if (interfaceDeclaration && symbol) interfaceDeclaration->provider = symbol;
}

SemanticModel SemanticAnalyzer::Analyze(core::CompilationUnit& unit) {
    _model = SemanticModel{};
    const auto diagnosticsBeforeExpansion = _diagnostics.Diagnostics().size();
    ExpandGenerics(unit, _diagnostics);
    if (_diagnostics.Diagnostics().size() > diagnosticsBeforeExpansion &&
        _diagnostics.HasErrors()) {
        ReportGenericOrigins(unit, _diagnostics);
        return std::move(_model);
    }
    _currentFunction = nullptr;
    _inUnsafe = false;
    _inZero = false;
    _zeroTrivialityKnown = false;
    _directConstructions.clear();
    _callableSignatures.clear();
    _warnedDefaults.clear();
    _zeroDependencies.clear();
    _functionCalls.clear();
    _crashableCalls.clear();
    _directCrashFunctions.clear();
    _forbiddenZeroEffects.clear();
    _implicitDefaults.clear();
    _buildSelf = nullptr;
    _allowBuildSelf = false;
    _buildDepth = 0;
    _analyzingCallCallee = false;
    _callCalleeCrashable = false;
    _currentScope = &AddScope(nullptr);
    RegisterBuiltIns();
    _moduleScopes.clear();
    _sourceScopes.clear();
    _sourceModules.clear();
    _inaccessibleImports.clear();
    _ambiguousImports.clear();
    _quiverNames.clear();
    _entryModules.clear();
    _testModules.clear();
    _testingModule = unit.testingModule;
    _currentModule.clear();
    _currentSource.clear();
    Scope* builtins = _currentScope;
    _moduleScopes[""] = builtins;
    for (const auto& module : unit.modules) {
        _quiverNames.insert_or_assign(module.quiverId, module.quiverName);
        if (!module.id.empty() && !_moduleScopes.contains(module.id))
            _moduleScopes[module.id] = &AddScope(builtins);
    }
    for (const auto& module : unit.modules) {
        _sourceModules.insert_or_assign(module.sourceId, module.id);
        if (!_sourceScopes.contains(module.sourceId))
            _sourceScopes[module.sourceId] =
                &AddScope(_moduleScopes.at(module.id));
        if (module.entry) _entryModules.insert(module.id);
        if (module.test) _testModules.insert(module.id);
        for (const auto& import : module.imports)
            for (const auto& declaration : unit.Declarations()) {
                if (declaration->moduleId != module.id) continue;
                std::string name;
                if (declaration->Kind() ==
                    core::AstNodeKind::FunctionDeclaration) {
                    const auto& function =
                        static_cast<const core::FunctionDeclaration&>(
                            *declaration);
                    if (function.Owner().empty()) name = function.Name();
                }
                if (declaration->Kind() ==
                    core::AstNodeKind::StructureDeclaration)
                    name = static_cast<const core::StructureDeclaration&>(
                               *declaration)
                               .Name();
                if (declaration->Kind() ==
                    core::AstNodeKind::InterfaceDeclaration)
                    name = static_cast<const core::InterfaceDeclaration&>(
                               *declaration)
                               .Name();
                if (declaration->Kind() == core::AstNodeKind::EnumDeclaration)
                    name =
                        static_cast<const core::EnumDeclaration&>(*declaration)
                            .Name();
                if (declaration->Kind() == core::AstNodeKind::ErrorDeclaration)
                    name =
                        static_cast<const core::ErrorDeclaration&>(*declaration)
                            .Name();
                if (declaration->Kind() ==
                    core::AstNodeKind::CallableTypeDeclaration)
                    name = static_cast<const core::CallableTypeDeclaration&>(
                               *declaration)
                               .Name();
                if (declaration->Kind() ==
                    core::AstNodeKind::ModuleConstantDeclaration)
                    name = static_cast<const core::ModuleConstantDeclaration&>(
                               *declaration)
                               .Name();
                if (declaration->Kind() ==
                    core::AstNodeKind::DecorationDeclaration)
                    name = static_cast<const core::DecorationDeclaration&>(
                               *declaration)
                               .Name();
                if (name == import.alias)
                    Report(diagnostics::DiagnosticCode::DuplicateDeclaration,
                           import.range,
                           "import alias conflicts with declaration '" + name +
                               "'");
            }
    }
    const auto bindGenericAliases = [&] {
        for (const auto& [alias, declaration] : unit.genericAliases)
            if (const auto* symbol = _model.DeclaredSymbol(*declaration)) {
                const bool crashable =
                    declaration->Kind() ==
                        core::AstNodeKind::FunctionDeclaration &&
                    static_cast<const core::FunctionDeclaration&>(*declaration)
                        .IsCrashable();
                (void)_model._scopes.front()->DeclareAs(
                    EffectName(alias, crashable), *symbol);
            }
    };
    RegisterErrorTypes(unit);
    RegisterInterfaces(unit);
    RegisterEnums(unit);
    RegisterStructures(unit);
    DeclareCallableTypes(unit);
    RegisterModuleConstants(unit);
    bindGenericAliases();
    BindImports(unit);
    ResolveNominalRelations();
    ResolveInterfaceMethods();
    ResolveStructureFields();
    ValidateDecorations(unit);
    ResolveCallableTypes(unit);
    RegisterFunctions(unit);
    bindGenericAliases();
    BindImports(unit);
    AnalyzeModuleConstants(unit);
    GenerateInitializers();
    GenerateJsonMethods();
    BindImports(unit);
    ValidateInterfaceConformance();
    ValidateExports(unit);
    for (const auto& structure : _model._structures)
        for (const auto& field : structure->fields)
            if (field.syntax->Type().TypeForm() == core::TypeKind::Pointer &&
                field.syntax->Type().Contract() ==
                    core::OwnershipContract::Own) {
                if (!structure->tear)
                    Report(diagnostics::DiagnosticCode::InvalidLifecycle,
                           field.syntax->Range(),
                           "an owning field requires an explicit tear that "
                           "discharges its allocation");
                if (field.syntax->Initializer())
                    Report(diagnostics::DiagnosticCode::UnverifiedOwnership,
                           field.syntax->Range(),
                           "initialize owning fields in build; declaration "
                           "initializers are not yet supported");
            }

    for (const auto& structure : _model._structures) {
        _currentFunction = structure->init;
        _inZero = false;
        for (const auto& field : structure->fields)
            if (field.syntax->Initializer()) {
                SetSource(structure->syntax->sourceId);
                MarkDirectConstruction(_directConstructions,
                                       *field.syntax->Initializer(), true);
                const SemanticType actual =
                    AnalyzeExpression(*field.syntax->Initializer(), field.type);
                MarkDirectConstruction(_directConstructions,
                                       *field.syntax->Initializer(), false);
                if (!IsAssignable(field.type, actual))
                    Report(diagnostics::DiagnosticCode::IncompatibleType,
                           field.syntax->Initializer()->Range(),
                           TypeMismatch(field.type, actual));
                CheckTransfer(*field.syntax->Initializer(), field.type);
            }
    }
    _currentFunction = nullptr;
    for (const auto& function : _model._generatedFunctions)
        AnalyzeDefaultArguments(*function);
    for (const std::unique_ptr<core::Declaration>& declaration :
         unit.Declarations())
        if (declaration->Kind() == core::AstNodeKind::FunctionDeclaration)
            AnalyzeDefaultArguments(
                static_cast<const core::FunctionDeclaration&>(*declaration));
    for (const std::unique_ptr<core::Declaration>& declaration :
         unit.Declarations())
        if (declaration->Kind() == core::AstNodeKind::FunctionDeclaration &&
            static_cast<const core::FunctionDeclaration&>(*declaration)
                .HasVariadicPack())
            AnalyzeFunction(
                static_cast<const core::FunctionDeclaration&>(*declaration));
    for (const auto& declaration : unit.Declarations()) {
        if (declaration->Kind() != core::AstNodeKind::FunctionDeclaration)
            continue;
        const auto& syntax =
            static_cast<const core::FunctionDeclaration&>(*declaration);
        if (!syntax.HasVariadicPack()) continue;
        const auto* start =
            static_cast<const FunctionSymbol*>(_model.DeclaredSymbol(syntax));
        if (start == nullptr) continue;
        std::unordered_set<const FunctionSymbol*> seen;
        const FunctionSymbol* current = start;
        while (current != nullptr && seen.insert(current).second) {
            const auto next = _model._variadicTargets.find(current);
            current =
                next == _model._variadicTargets.end() ? nullptr : next->second;
        }
        if (current != nullptr && current->Declaration() != nullptr)
            Report(
                diagnostics::DiagnosticCode::InvalidVariadic,
                start->Declaration()->VariadicPackRange(),
                "variadic forwarding cycle involving '" + start->Name() + "'");
    }
    for (const auto& function : _model._generatedFunctions)
        AnalyzeFunction(*function);
    for (const std::unique_ptr<core::Declaration>& declaration :
         unit.Declarations()) {
        if (declaration->Kind() == core::AstNodeKind::FunctionDeclaration) {
            const auto& function =
                static_cast<const core::FunctionDeclaration&>(*declaration);
            if (!function.HasVariadicPack()) AnalyzeFunction(function);
        }
    }
    // Anonymous functions are discovered while their enclosing bodies are
    // analyzed. Keep using an index because analyzing one anonymous function
    // can discover another nested anonymous function.
    for (std::size_t index = 0; index < _model._anonymousFunctions.size();
         ++index)
        AnalyzeFunction(*_model._anonymousFunctions[index]);
    ClassifyTrivialZeros(unit);
    _zeroTrivialityKnown = true;
    for (const auto& [declaration, type] : _implicitDefaults) {
        if (!type.IsError() && !type.IsTriviallyZeroable())
            Report(diagnostics::DiagnosticCode::InvalidVariableType,
                   declaration->NameRange(),
                   "a declaration without an initializer requires a "
                   "trivially zeroable type");
    }
    for (const auto& structure : _model._structures)
        if (SemanticType(structure.get()).IsZeroable())
            WarnForNontrivialDefault(SemanticType(structure.get()),
                                     structure->syntax->NameRange());
    ValidateZeroCycles();
    ValidateZeroEffects();
    WarnForCrashlessFunctions();
    if (!_diagnostics.HasErrors())
        AnalyzeAllocations(unit, _model, _diagnostics);
    ReportGenericOrigins(unit, _diagnostics);
    _currentScope = nullptr;
    _currentFunction = nullptr;
    return std::move(_model);
}

void SemanticAnalyzer::AnalyzeDefaultArguments(
    const core::FunctionDeclaration& function) {
    if (function.Name().starts_with("$anon")) return;
    const Symbol* symbol = _model.DeclaredSymbol(function);
    if (symbol == nullptr || symbol->Kind() != SymbolKind::Function) return;
    const auto* functionSymbol = static_cast<const FunctionSymbol*>(symbol);
    SetSource(function.sourceId);
    const FunctionSymbol* previousFunction = _currentFunction;
    const bool previousUnsafe = _inUnsafe;
    const bool previousZero = _inZero;
    _currentFunction = functionSymbol;
    _inUnsafe = function.IsUnsafe();
    _inZero =
        functionSymbol->Owner() && functionSymbol->Name().ends_with("::zero");
    for (std::size_t index = 0; index < function.Parameters().size(); ++index) {
        const core::Expression* value =
            function.Parameters()[index]->DefaultValue();
        if (value == nullptr) continue;
        const SemanticType expected = functionSymbol->ParameterTypes()[index];
        const SemanticType actual = AnalyzeExpression(*value, expected);
        CheckTransfer(*value, expected,
                      functionSymbol->ParameterContract(index) ==
                          core::OwnershipContract::Own);
        if (!IsAssignable(expected, actual) && !CanReborrow(expected, actual))
            Report(diagnostics::DiagnosticCode::InvalidDefaultArgument,
                   value->Range(), TypeMismatch(expected, actual));
    }
    _currentFunction = previousFunction;
    _inUnsafe = previousUnsafe;
    _inZero = previousZero;
}

void SemanticAnalyzer::AnalyzeFunction(
    const core::FunctionDeclaration& function) {
    SetSource(function.sourceId);
    const std::size_t diagnosticsBefore = _diagnostics.Diagnostics().size();
    _buildSelf = nullptr;
    _buildDepth = 0;
    const Symbol* functionBase = _model.DeclaredSymbol(function);
    if (functionBase != nullptr &&
        functionBase->Kind() == SymbolKind::Function) {
        _currentFunction = static_cast<const FunctionSymbol*>(functionBase);
    } else {
        _currentFunction = nullptr;
    }
    _inZero = _currentFunction && _currentFunction->Owner() &&
              _currentFunction->Name().ends_with("::zero");

    if (_currentFunction)
        for (const auto& type : _currentFunction->ParameterTypes())
            if (type.IsImmovable())
                Report(diagnostics::DiagnosticCode::NonCopyableValue,
                       function.NameRange(),
                       "immovable structure parameters must be borrowed");
    _positions.clear();
    _localStringBorrows.clear();
    _loopDepth = 0;
    _inUnsafe = function.IsUnsafe();
    CollectPositions(function.Body(), 0);

    Scope* previousScope = _currentScope;
    _currentScope = &AddScope(previousScope);
    for (const ClosureCapture& capture : _model.Captures(function)) {
        if (!_currentScope->Declare(*capture.variable))
            Report(
                diagnostics::DiagnosticCode::DuplicateDeclaration,
                capture.syntax->NameRange(),
                "duplicate declaration of '" + capture.variable->Name() + "'");
    }
    std::size_t parameterIndex = 0;
    for (const std::unique_ptr<core::Parameter>& parameter :
         function.Parameters()) {
        AddSymbol(std::make_unique<VariableSymbol>(
                      SymbolKind::Parameter, parameter->Name(),
                      _currentFunction->ParameterTypes()[parameterIndex++],
                      parameter->NameRange(), !parameter->Type().IsConst()),
                  *parameter);
    }
    if (function.HasVariadicPack()) {
        AnalyzeVariadicFunction(function);
        _currentScope = previousScope;
        _currentFunction = nullptr;
        _inUnsafe = false;
        _inZero = false;
        return;
    }
    if (function.IsExtern()) {
        _currentScope = previousScope;
        _currentFunction = nullptr;
        _inUnsafe = false;
        _inZero = false;
        return;
    }
    AnalyzeBlock(function.Body(), false);
    const ControlFlowGraph graph = BuildControlFlowGraph(function, _model);
    if (_currentFunction &&
        _currentFunction->ReturnType().Kind() != SemanticTypeKind::Void &&
        !_currentFunction->ReturnType().IsError() &&
        CanReachFunctionExit(graph)) {
        Report(diagnostics::DiagnosticCode::InvalidReturnType,
               function.NameRange(),
               "not all control paths in function '" + function.Name() +
                   "' return '" + _currentFunction->ReturnType().Name() + "'");
    }
    AnalyzeDefiniteAssignment(function, _model, _diagnostics, graph);
    bool hasNewErrors = false;
    for (std::size_t index = diagnosticsBefore;
         index < _diagnostics.Diagnostics().size(); ++index)
        if (_diagnostics.Diagnostics()[index].severity ==
            diagnostics::DiagnosticSeverity::Error) {
            hasNewErrors = true;
            break;
        }
    if (!hasNewErrors) AnalyzeBorrows(function, _model, _diagnostics, graph);
    _currentScope = previousScope;
    _currentFunction = nullptr;
    _inUnsafe = false;
    _inZero = false;
}

void SemanticAnalyzer::AnalyzeVariadicFunction(
    const core::FunctionDeclaration& function) {
    const auto invalid = [&](source::SourceRange range, std::string message) {
        Report(diagnostics::DiagnosticCode::InvalidVariadic, range,
               std::move(message));
    };
    if (_currentFunction == nullptr) return;

    const auto& statements = function.Body().Statements();
    if (statements.size() != 1) {
        invalid(function.Body().Range(),
                "a variadic function body must contain exactly one "
                "forwarding call");
        return;
    }

    const core::Expression* value = nullptr;
    if (_currentFunction->ReturnType().Kind() == SemanticTypeKind::Void &&
        statements[0]->Kind() == core::AstNodeKind::ExpressionStatement) {
        value = &static_cast<const core::ExpressionStatement&>(*statements[0])
                     .Value();
    } else if (_currentFunction->ReturnType().Kind() !=
                   SemanticTypeKind::Void &&
               statements[0]->Kind() == core::AstNodeKind::ReturnStatement) {
        value =
            static_cast<const core::ReturnStatement&>(*statements[0]).Value();
    }
    if (value == nullptr ||
        value->Kind() != core::AstNodeKind::CallExpression) {
        invalid(statements[0]->Range(),
                _currentFunction->ReturnType().Kind() == SemanticTypeKind::Void
                    ? "a void variadic function must contain one forwarding "
                      "call statement"
                    : "a non-void variadic function must return one "
                      "forwarding call");
        return;
    }

    const auto& call = static_cast<const core::CallExpression&>(*value);
    if (call.Callee().Kind() != core::AstNodeKind::NameExpression ||
        call.TypeArgument() != nullptr) {
        invalid(call.Callee().Range(),
                "a variadic pack can currently forward only to a named "
                "function");
        return;
    }
    if (call.Arguments().size() != function.Parameters().size() + 1) {
        invalid(call.Range(),
                "a forwarding call must pass every fixed parameter in order "
                "followed by the expanded pack");
        return;
    }
    for (std::size_t index = 0; index < function.Parameters().size(); ++index) {
        const auto& argument = *call.Arguments()[index];
        if (call.ArgumentIsExpanded(index) ||
            argument.Kind() != core::AstNodeKind::NameExpression ||
            static_cast<const core::NameExpression&>(argument).Name() !=
                function.Parameters()[index]->Name()) {
            invalid(argument.Range(),
                    "a forwarding call must pass fixed parameter '" +
                        function.Parameters()[index]->Name() +
                        "' unchanged and in order");
            return;
        }
    }
    const std::size_t packIndex = function.Parameters().size();
    const auto& pack = *call.Arguments()[packIndex];
    if (!call.ArgumentIsExpanded(packIndex) ||
        pack.Kind() != core::AstNodeKind::NameExpression ||
        static_cast<const core::NameExpression&>(pack).Name() !=
            function.VariadicPackName()) {
        invalid(pack.Range(),
                "the final forwarding argument must be the expanded pack '" +
                    function.VariadicPackName() + "...'");
        return;
    }

    const auto& callee =
        static_cast<const core::NameExpression&>(call.Callee());
    const Symbol* targetBase =
        _currentScope->Lookup(EffectName(callee.Name(), call.IsCrashable()));
    if (targetBase == nullptr || targetBase->Kind() != SymbolKind::Function) {
        invalid(callee.Range(), "variadic forwarding target '" + callee.Name() +
                                    "' is not a function");
        return;
    }
    const auto* target = static_cast<const FunctionSymbol*>(targetBase);
    ValidateCrashableCall(call.IsCrashable(), target->Signature(), target,
                          call.Range());
    if (target == _currentFunction) {
        invalid(callee.Range(), "a variadic function cannot forward to itself");
        return;
    }
    if (!IsAssignable(_currentFunction->ReturnType(), target->ReturnType()) ||
        !IsAssignable(target->ReturnType(), _currentFunction->ReturnType())) {
        invalid(callee.Range(),
                "forwarding target returns '" +
                    std::string(target->ReturnType().Name()) +
                    "', but the variadic function returns '" +
                    std::string(_currentFunction->ReturnType().Name()) + "'");
    }
    if (target->IsUnsafe() && !_currentFunction->IsUnsafe())
        invalid(callee.Range(),
                "a safe variadic function cannot forward to an unsafe "
                "function");
    if (target->BuiltIn() == BuiltInFunction::None) {
        const std::size_t shared =
            std::min(_currentFunction->ParameterTypes().size(),
                     target->ParameterTypes().size());
        for (std::size_t index = 0; index < shared; ++index) {
            const SemanticType source =
                _currentFunction->ParameterTypes()[index];
            const SemanticType destination = target->ParameterTypes()[index];
            if (!IsAssignable(destination, source) ||
                !IsAssignable(source, destination))
                invalid(call.Arguments()[index]->Range(),
                        "fixed parameter '" +
                            function.Parameters()[index]->Name() +
                            "' has type '" + std::string(source.Name()) +
                            "', but the forwarding target expects '" +
                            std::string(destination.Name()) + "'");
            if (_currentFunction->ParameterContract(index) !=
                target->ParameterContract(index))
                invalid(call.Arguments()[index]->Range(),
                        "fixed parameter ownership must match the forwarding "
                        "target");
        }
        for (std::size_t index = shared;
             index < target->ParameterTypes().size(); ++index)
            if (target->ParameterContract(index) !=
                core::OwnershipContract::None)
                invalid(pack.Range(),
                        "an untyped variadic pack cannot supply an ownership-"
                        "contract parameter");
    }
    for (const std::size_t targetSource : target->BorrowSourceIndices()) {
        const bool declared =
            targetSource < _currentFunction->ParameterTypes().size()
                ? std::find(_currentFunction->BorrowSourceIndices().begin(),
                            _currentFunction->BorrowSourceIndices().end(),
                            targetSource) !=
                      _currentFunction->BorrowSourceIndices().end()
                : _currentFunction->BorrowsVariadicPack();
        if (!declared)
            invalid(callee.Range(),
                    "the variadic function borrow contract must include every "
                    "source borrowed by its forwarding target");
    }
    if (target->BorrowsVariadicPack() &&
        !_currentFunction->BorrowsVariadicPack())
        invalid(callee.Range(),
                "the variadic function must declare '..." +
                    function.VariadicPackName() +
                    "' as a borrow source because its target borrows the "
                    "forwarded pack");

    _model._bindings.insert_or_assign(&callee, target);
    _model._types.insert_or_assign(&callee, target->Type());
    _model._types.insert_or_assign(&call, target->ReturnType().WithoutConst());
    _model._variadicTargets.insert_or_assign(_currentFunction, target);
    _functionCalls[_currentFunction].insert(target);
}

void SemanticAnalyzer::CollectPositions(const core::BlockStatement& block,
                                        std::size_t loopDepth) {
    for (const std::unique_ptr<core::Statement>& statement :
         block.Statements()) {
        if (statement->Kind() == core::AstNodeKind::BlockStatement) {
            CollectPositions(
                static_cast<const core::BlockStatement&>(*statement),
                loopDepth);
            continue;
        }
        if (statement->Kind() == core::AstNodeKind::RangeStatement) {
            CollectPositions(
                static_cast<const core::RangeStatement&>(*statement)
                    .Expansion(),
                loopDepth);
            continue;
        }
        if (statement->Kind() == core::AstNodeKind::PositionStatement) {
            const core::PositionStatement& position =
                static_cast<const core::PositionStatement&>(*statement);
            if (loopDepth == 0) {
                Report(diagnostics::DiagnosticCode::LoopControlOutsideLoop,
                       position.Range(),
                       "positions are only valid inside a loop");
            }
            if (!_positions.emplace(position.Name(), position.NameRange())
                     .second) {
                Report(diagnostics::DiagnosticCode::DuplicatePosition,
                       position.NameRange(),
                       "duplicate position '" + position.Name() + "'");
            }
            continue;
        }
        if (statement->Kind() == core::AstNodeKind::IfStatement) {
            const core::IfStatement& ifStatement =
                static_cast<const core::IfStatement&>(*statement);
            CollectPositions(ifStatement.PrimaryBranch().Body(), loopDepth);
            if (ifStatement.PrimaryBranch().Also() != nullptr) {
                CollectPositions(ifStatement.PrimaryBranch().Also()->Body(),
                                 loopDepth);
            }
            for (const std::unique_ptr<core::ConditionalBranch>& branch :
                 ifStatement.ElseIfBranches()) {
                CollectPositions(branch->Body(), loopDepth);
                if (branch->Also() != nullptr) {
                    CollectPositions(branch->Also()->Body(), loopDepth);
                }
            }
            if (ifStatement.ElseBody() != nullptr) {
                CollectPositions(*ifStatement.ElseBody(), loopDepth);
            }
            continue;
        }
        if (statement->Kind() == core::AstNodeKind::WhileStatement) {
            const core::WhileStatement& whileStatement =
                static_cast<const core::WhileStatement&>(*statement);
            CollectPositions(whileStatement.Body(), loopDepth + 1);
            if (whileStatement.Also() != nullptr) {
                CollectPositions(whileStatement.Also()->Body(), loopDepth + 1);
            }
        }
    }
}

void SemanticAnalyzer::AnalyzeBlock(const core::BlockStatement& block,
                                    bool createScope) {
    Scope* previousScope = _currentScope;
    if (createScope) _currentScope = &AddScope(previousScope);
    for (const std::unique_ptr<core::Statement>& statement :
         block.Statements()) {
        AnalyzeStatement(*statement);
    }
    if (createScope) _currentScope = previousScope;
}

void SemanticAnalyzer::AnalyzeBranch(const core::ConditionalBranch& branch) {
    const SemanticType condition = AnalyzeExpression(branch.Condition());
    if (!condition.IsError() && condition.Kind() != SemanticTypeKind::Bool) {
        Report(diagnostics::DiagnosticCode::InvalidCondition,
               branch.Condition().Range(), "condition must have type 'bool'");
    }
    AnalyzeBlock(branch.Body(), true);
    if (branch.Also() != nullptr) {
        const SemanticType alsoCondition =
            AnalyzeExpression(branch.Also()->Condition());
        if (!alsoCondition.IsError() &&
            alsoCondition.Kind() != SemanticTypeKind::Bool) {
            Report(diagnostics::DiagnosticCode::InvalidCondition,
                   branch.Also()->Condition().Range(),
                   "condition must have type 'bool'");
        }
        AnalyzeBlock(branch.Also()->Body(), true);
    }
}

void SemanticAnalyzer::AnalyzeStatement(const core::Statement& statement) {
    switch (statement.Kind()) {
        case core::AstNodeKind::BlockStatement: {
            const auto& block =
                static_cast<const core::BlockStatement&>(statement);
            if (_inZero && block.IsUnsafe())
                Report(diagnostics::DiagnosticCode::InvalidLifecycle,
                       block.Range(), "zero cannot contain unsafe operations");
            if (block.IsUnsafe() && _currentFunction)
                _forbiddenZeroEffects.insert(_currentFunction);
            const bool previousUnsafe = _inUnsafe;
            _inUnsafe = _inUnsafe || block.IsUnsafe();
            AnalyzeBlock(block, true);
            _inUnsafe = previousUnsafe;
            break;
        }
        case core::AstNodeKind::DestructureStatement:
            AnalyzeDestructure(
                static_cast<const core::DestructureStatement&>(statement));
            break;
        case core::AstNodeKind::VariableDeclarationStatement: {
            const core::VariableDeclarationStatement& variable =
                static_cast<const core::VariableDeclarationStatement&>(
                    statement);
            const SemanticType declaredType =
                variable.HasExplicitType()
                    ? ResolveType(*variable.ExplicitType())
                    : SemanticType::Error();
            SemanticType initializerType = SemanticType::Error();
            if (variable.Initializer() != nullptr) {
                MarkDirectConstruction(_directConstructions,
                                       *variable.Initializer(), true);
                initializerType =
                    AnalyzeExpression(*variable.Initializer(), declaredType);
                MarkDirectConstruction(_directConstructions,
                                       *variable.Initializer(), false);
            }
            SemanticType variableType = initializerType.WithoutConst();
            if (variable.HasExplicitType()) {
                variableType = declaredType;
                if (variable.IsImplicitlyDefaulted())
                    _implicitDefaults.emplace_back(&variable, variableType);
                if (variableType.Kind() == SemanticTypeKind::Void) {
                    Report(diagnostics::DiagnosticCode::InvalidVariableType,
                           variable.ExplicitType()->Range(),
                           "a variable cannot have type 'void'");
                } else if (variable.Initializer() != nullptr &&
                           !IsAssignable(variableType, initializerType)) {
                    Report(diagnostics::DiagnosticCode::IncompatibleType,
                           variable.Initializer()->Range(),
                           TypeMismatch(variableType, initializerType));
                }
            } else if (initializerType.IsError() ||
                       initializerType.Kind() == SemanticTypeKind::Void) {
                Report(diagnostics::DiagnosticCode::CannotInferType,
                       variable.NameRange(),
                       "cannot infer a variable type for '" + variable.Name() +
                           "'");
                variableType = SemanticType::Error();
            }
            if (variable.Initializer())
                CheckTransfer(*variable.Initializer(), variableType);
            if (variableType.Kind() == SemanticTypeKind::Tuple &&
                HasBorrowedResult(variableType))
                Report(diagnostics::DiagnosticCode::InvalidBorrowContract,
                       variable.NameRange(),
                       "a tuple containing references or slices must be "
                       "immediately destructured");
            if (_currentFunction && variable.Initializer() &&
                variableType.Kind() == SemanticTypeKind::String &&
                initializerType.Kind() != SemanticTypeKind::String &&
                initializerType.Kind() != SemanticTypeKind::None)
                _forbiddenZeroEffects.insert(_currentFunction);
            if (variableType.ContainsStructure() && !variable.Initializer())
                Report(diagnostics::DiagnosticCode::InvalidStructure,
                       variable.NameRange(),
                       "structure storage requires an initializer");
            if (variable.IsRangeSource() &&
                variableType.Kind() == SemanticTypeKind::Slice)
                variableType = SemanticType(SemanticTypeKind::Slice,
                                            variableType.Target()->WithConst());
            if (variableType.IsBorrowed() &&
                variable.Initializer() == nullptr) {
                Report(
                    diagnostics::DiagnosticCode::ReferenceRequiresInitializer,
                    variable.NameRange(),
                    "a reference or slice requires an initializer");
            }
            if (variableType.IsConst() && variable.Initializer() == nullptr &&
                !variableType.IsBorrowed()) {
                Report(diagnostics::DiagnosticCode::ConstRequiresInitializer,
                       variable.NameRange(),
                       "a const declaration requires an initializer");
            }
            const Symbol* symbol = AddSymbol(
                std::make_unique<VariableSymbol>(
                    SymbolKind::LocalVariable, variable.Name(), variableType,
                    variable.NameRange(), !variableType.IsConst()),
                variable);
            if (symbol != nullptr) RecordCleanupEffects(variableType);
            if (symbol != nullptr && variable.Initializer() != nullptr &&
                variableType.Kind() == SemanticTypeKind::StringView &&
                BorrowsLocalString(*variable.Initializer())) {
                _localStringBorrows.insert(symbol);
            }
            if (symbol != nullptr && variable.Initializer() == nullptr &&
                variableType.Kind() == SemanticTypeKind::StringView) {
                Report(
                    diagnostics::DiagnosticCode::BorrowRequiresInitializer,
                    variable.NameRange(),
                    "a borrowed 'stringv' declaration requires an initializer");
            }
            break;
        }
        case core::AstNodeKind::DeferStatement: {
            const auto& deferred =
                static_cast<const core::DeferStatement&>(statement);
            const core::Expression* expression = &deferred.Call();
            while (expression->Kind() ==
                   core::AstNodeKind::ParenthesizedExpression)
                expression = &static_cast<const core::ParenthesizedExpression&>(
                                  *expression)
                                  .Inner();
            AnalyzeExpression(deferred.Call());
            if (expression->Kind() != core::AstNodeKind::CallExpression &&
                expression->Kind() != core::AstNodeKind::MemberCallExpression) {
                Report(diagnostics::DiagnosticCode::InvalidDefer,
                       expression->Range(),
                       "defer requires a function, callable, or method call");
                break;
            }

            const FunctionSymbol* function = nullptr;
            std::vector<std::pair<const core::Expression*, SemanticType>>
                operands;
            if (expression->Kind() == core::AstNodeKind::CallExpression) {
                const auto& call =
                    static_cast<const core::CallExpression&>(*expression);
                const SemanticType calleeType = _model.TypeOf(call.Callee());
                if (call.Callee().Kind() == core::AstNodeKind::NameExpression) {
                    const Symbol* symbol = _model.SymbolFor(
                        static_cast<const core::NameExpression&>(
                            call.Callee()));
                    if (symbol != nullptr &&
                        symbol->Kind() == SymbolKind::Function)
                        function = static_cast<const FunctionSymbol*>(symbol);
                }
                if (function == nullptr) {
                    if (calleeType.Kind() == SemanticTypeKind::Function) {
                        CheckTransfer(call.Callee(), calleeType);
                        operands.emplace_back(&call.Callee(), calleeType);
                    }
                }
                const auto* signature =
                    calleeType.Kind() == SemanticTypeKind::Function
                        ? &calleeType.Signature()
                        : nullptr;
                const auto arguments = _model.ArgumentsFor(call);
                for (std::size_t index = 0; index < arguments.size(); ++index) {
                    const SemanticType type =
                        signature != nullptr &&
                                (!function || function->BuiltIn() ==
                                                  BuiltInFunction::None) &&
                                index < signature->parameters.size()
                            ? signature->parameters[index]
                            : _model.TypeOf(*arguments[index]);
                    operands.emplace_back(arguments[index], type);
                }
            } else {
                const auto& call =
                    static_cast<const core::MemberCallExpression&>(*expression);
                function = _model.MethodFor(call);
                if (function == nullptr) {
                    Report(diagnostics::DiagnosticCode::InvalidDefer,
                           expression->Range(),
                           "defer requires a structure method call");
                    break;
                }
                const std::size_t offset = function->HasReceiver() ? 1 : 0;
                if (offset != 0 && !function->ParameterTypes().empty())
                    operands.emplace_back(&call.Receiver(),
                                          function->ParameterTypes()[0]);
                const auto arguments = _model.ArgumentsFor(call);
                for (std::size_t index = 0; index < arguments.size(); ++index) {
                    const SemanticType type =
                        index + offset < function->ParameterTypes().size()
                            ? function->ParameterTypes()[index + offset]
                            : _model.TypeOf(*arguments[index]);
                    operands.emplace_back(arguments[index], type);
                }
            }

            if (function != nullptr &&
                (function->BuiltIn() == BuiltInFunction::New ||
                 function->BuiltIn() == BuiltInFunction::Take ||
                 function->BuiltIn() == BuiltInFunction::Hash ||
                 function->BuiltIn() == BuiltInFunction::StorageNew ||
                 function->BuiltIn() == BuiltInFunction::StorageInitialize ||
                 function->BuiltIn() == BuiltInFunction::StorageRelocate ||
                 function->BuiltIn() == BuiltInFunction::StorageExtract ||
                 function->BuiltIn() == BuiltInFunction::StorageDestroy ||
                 function->BuiltIn() == BuiltInFunction::StorageFree ||
                 function->BuiltIn() == BuiltInFunction::JsonSliceAppend)) {
                Report(diagnostics::DiagnosticCode::InvalidDefer,
                       expression->Range(),
                       "compiler intrinsics cannot be deferred");
                break;
            }
            std::vector<const Symbol*>& captures =
                _model._deferredCaptures[&deferred];
            for (std::size_t index = 0; index < operands.size(); ++index) {
                const auto [operand, type] = operands[index];
                const Symbol* capture = nullptr;
                if (type.Kind() != SemanticTypeKind::StringView &&
                    CanCarryBorrow(type)) {
                    auto owned = std::make_unique<VariableSymbol>(
                        SymbolKind::LocalVariable,
                        "deferred operand " + std::to_string(index + 1), type,
                        operand->Range(), false);
                    capture = owned.get();
                    _model._symbols.push_back(std::move(owned));
                }
                captures.push_back(capture);
            }
            break;
        }
        case core::AstNodeKind::ExpressionStatement: {
            const auto& value =
                static_cast<const core::ExpressionStatement&>(statement)
                    .Value();
            const SemanticType type = AnalyzeExpression(value);
            const core::Expression* effect = &value;
            while (effect->Kind() == core::AstNodeKind::ParenthesizedExpression)
                effect =
                    &static_cast<const core::ParenthesizedExpression&>(*effect)
                         .Inner();
            if (type.ContainsStructure() && !type.IsCopyable() &&
                effect->Kind() != core::AstNodeKind::AssignmentExpression)
                CheckTransfer(value, type);
            if (type.Kind() == SemanticTypeKind::Tuple &&
                HasBorrowedResult(type))
                Report(diagnostics::DiagnosticCode::InvalidBorrowContract,
                       value.Range(),
                       "a borrowed tuple result must be immediately "
                       "destructured");
            break;
        }
        case core::AstNodeKind::ReturnStatement: {
            const core::ReturnStatement& returnStatement =
                static_cast<const core::ReturnStatement&>(statement);
            const SemanticType expected = _currentFunction == nullptr
                                              ? SemanticType::Error()
                                              : _currentFunction->ReturnType();
            SemanticType actual(SemanticTypeKind::Void);
            if (returnStatement.Value()) {
                MarkDirectConstruction(_directConstructions,
                                       *returnStatement.Value(), true);
                actual = AnalyzeExpression(*returnStatement.Value(), expected);
                MarkDirectConstruction(_directConstructions,
                                       *returnStatement.Value(), false);
            }
            if (returnStatement.Value()) {
                const bool consumingReturn =
                    _currentFunction != nullptr &&
                    expected.Kind() != SemanticTypeKind::Tuple &&
                    !_currentFunction->Signature().resultContracts.empty() &&
                    _currentFunction->Signature().resultContracts.front() ==
                        core::OwnershipContract::Own;
                CheckTransfer(*returnStatement.Value(), expected,
                              consumingReturn);
            }
            if (_currentFunction && returnStatement.Value() &&
                expected.Kind() == SemanticTypeKind::String &&
                actual.Kind() != SemanticTypeKind::String &&
                actual.Kind() != SemanticTypeKind::None)
                _forbiddenZeroEffects.insert(_currentFunction);
            if (_buildDepth != 0)
                Report(diagnostics::DiagnosticCode::InvalidStructure,
                       returnStatement.Range(),
                       "return cannot leave a build block; validate before "
                       "constructing");
            if (!IsAssignable(expected, actual) &&
                !CanReborrow(expected, actual)) {
                const source::SourceRange range =
                    returnStatement.Value() == nullptr
                        ? returnStatement.Range()
                        : returnStatement.Value()->Range();
                Report(diagnostics::DiagnosticCode::InvalidReturnType, range,
                       TypeMismatch(expected, actual));
            } else if (returnStatement.Value() != nullptr &&
                       expected.Kind() == SemanticTypeKind::StringView &&
                       (_currentFunction == nullptr ||
                        _currentFunction->BorrowSourceIndices().empty()) &&
                       BorrowsLocalString(*returnStatement.Value())) {
                Report(diagnostics::DiagnosticCode::BorrowEscapesOwner,
                       returnStatement.Value()->Range(),
                       "borrowed string view outlives its local owning string");
            }
            break;
        }
        case core::AstNodeKind::IfStatement: {
            const core::IfStatement& ifStatement =
                static_cast<const core::IfStatement&>(statement);
            AnalyzeBranch(ifStatement.PrimaryBranch());
            for (const std::unique_ptr<core::ConditionalBranch>& branch :
                 ifStatement.ElseIfBranches()) {
                AnalyzeBranch(*branch);
            }
            if (ifStatement.ElseBody() != nullptr) {
                AnalyzeBlock(*ifStatement.ElseBody(), true);
            }
            break;
        }
        case core::AstNodeKind::RangeStatement: {
            const auto& range =
                static_cast<const core::RangeStatement&>(statement);
            AnalyzeBlock(range.Expansion(), true);
            const SemanticType source =
                _model.TypeOf(*range.Source().Initializer());
            const bool stringRange =
                source.Kind() == SemanticTypeKind::StringView ||
                source.Kind() == SemanticTypeKind::String;
            if (!source.IsError() && !source.IsSequence() && !stringRange)
                Report(diagnostics::DiagnosticCode::IncompatibleType,
                       range.Source().Range(),
                       "range requires a fixed array, slice, string, or "
                       "stringv value");
            if (range.IndexBinding() != nullptr) {
                const Symbol* symbol =
                    _model.DeclaredSymbol(*range.IndexBinding());
                if (symbol != nullptr && !symbol->Type().IsError()) {
                    const SemanticType index = symbol->Type();
                    if (!index.IsInteger()) {
                        Report(diagnostics::DiagnosticCode::IncompatibleType,
                               range.IndexBinding()->NameRange(),
                               "range index must have an integer type");
                    } else if (stringRange &&
                               index.Kind() != SemanticTypeKind::U32) {
                        Report(diagnostics::DiagnosticCode::IncompatibleType,
                               range.IndexBinding()->NameRange(),
                               "string range indices must use u32 because "
                               "string lengths are u32");
                    } else if (source.Kind() == SemanticTypeKind::Slice &&
                               index.Kind() != SemanticTypeKind::USize) {
                        Report(diagnostics::DiagnosticCode::IncompatibleType,
                               range.IndexBinding()->NameRange(),
                               "slice range indices must use usize because the "
                               "length is dynamic");
                    } else if (source.Kind() == SemanticTypeKind::Array &&
                               source.Length() != 0) {
                        // Reuse literal representability checks for the
                        // greatest exposed index. The hidden usize cursor
                        // cannot wrap.
                        const core::LiteralExpression last(
                            range.IndexBinding()->NameRange(),
                            TokenType::NumberLiteral,
                            std::to_string(source.Length() - 1));
                        AnalyzeLiteral(last, index);
                    }
                }
            }
            break;
        }
        case core::AstNodeKind::WhileStatement: {
            const core::WhileStatement& whileStatement =
                static_cast<const core::WhileStatement&>(statement);
            const SemanticType condition =
                AnalyzeExpression(whileStatement.Condition());
            if (!condition.IsError() &&
                condition.Kind() != SemanticTypeKind::Bool) {
                Report(diagnostics::DiagnosticCode::InvalidCondition,
                       whileStatement.Condition().Range(),
                       "condition must have type 'bool'");
            }
            ++_loopDepth;
            AnalyzeBlock(whileStatement.Body(), true);
            if (whileStatement.Update() != nullptr)
                AnalyzeExpression(*whileStatement.Update());
            if (whileStatement.Also() != nullptr) {
                const SemanticType alsoCondition =
                    AnalyzeExpression(whileStatement.Also()->Condition());
                if (!alsoCondition.IsError() &&
                    alsoCondition.Kind() != SemanticTypeKind::Bool) {
                    Report(diagnostics::DiagnosticCode::InvalidCondition,
                           whileStatement.Also()->Condition().Range(),
                           "condition must have type 'bool'");
                }
                AnalyzeBlock(whileStatement.Also()->Body(), true);
            }
            --_loopDepth;
            break;
        }
        case core::AstNodeKind::ProceedStatement:
        case core::AstNodeKind::BreakStatement:
            if (_loopDepth == 0) {
                Report(diagnostics::DiagnosticCode::LoopControlOutsideLoop,
                       statement.Range(),
                       statement.Kind() == core::AstNodeKind::ProceedStatement
                           ? "'proceed' is only valid inside a loop"
                           : "'break' is only valid inside a loop");
            }
            break;
        case core::AstNodeKind::JumpToStatement: {
            if (_buildDepth)
                Report(diagnostics::DiagnosticCode::InvalidStructure,
                       statement.Range(),
                       "jumpto cannot cross construction boundaries");
            const core::JumpToStatement& jump =
                static_cast<const core::JumpToStatement&>(statement);
            if (_loopDepth == 0) {
                Report(diagnostics::DiagnosticCode::LoopControlOutsideLoop,
                       statement.Range(),
                       "'jumpto' is only valid inside a loop");
            }
            if (!_positions.contains(jump.Position())) {
                Report(diagnostics::DiagnosticCode::UnknownPosition,
                       jump.PositionRange(),
                       "unknown position '" + jump.Position() + "'");
            }
            break;
        }
        case core::AstNodeKind::PositionStatement:
            if (_buildDepth)
                Report(diagnostics::DiagnosticCode::InvalidStructure,
                       statement.Range(),
                       "positions inside build blocks are not supported");
            break;
        default:
            break;
    }
}

SemanticType SemanticAnalyzer::AnalyzeExpression(
    const core::Expression& expression, SemanticType expected) {
    SemanticType type = SemanticType::Error();
    switch (expression.Kind()) {
        case core::AstNodeKind::AnonymousFunctionExpression:
            type = AnalyzeAnonymousFunction(
                static_cast<const core::AnonymousFunctionExpression&>(
                    expression),
                expected);
            break;
        case core::AstNodeKind::TupleExpression:
            type = AnalyzeTuple(
                static_cast<const core::TupleExpression&>(expression),
                expected);
            break;
        case core::AstNodeKind::ArrayExpression:
            type = AnalyzeArray(
                static_cast<const core::ArrayExpression&>(expression),
                expected);
            break;
        case core::AstNodeKind::MatchExpression:
            type = AnalyzeMatch(
                static_cast<const core::MatchExpression&>(expression),
                expected);
            break;
        case core::AstNodeKind::LiteralExpression:
            type = AnalyzeLiteral(
                static_cast<const core::LiteralExpression&>(expression),
                expected);
            break;
        case core::AstNodeKind::NameExpression:
            type = AnalyzeName(
                static_cast<const core::NameExpression&>(expression));
            break;
        case core::AstNodeKind::ParenthesizedExpression:
            type = AnalyzeExpression(
                static_cast<const core::ParenthesizedExpression&>(expression)
                    .Inner(),
                expected);
            break;
        case core::AstNodeKind::UnaryExpression:
            type = AnalyzeUnary(
                static_cast<const core::UnaryExpression&>(expression),
                expected);
            break;
        case core::AstNodeKind::BinaryExpression:
            type = AnalyzeBinary(
                static_cast<const core::BinaryExpression&>(expression),
                expected);
            break;
        case core::AstNodeKind::AssignmentExpression:
            type = AnalyzeAssignment(
                static_cast<const core::AssignmentExpression&>(expression));
            break;
        case core::AstNodeKind::CallExpression:
            type = AnalyzeCall(
                static_cast<const core::CallExpression&>(expression));
            break;
        case core::AstNodeKind::BuildExpression:
            type = AnalyzeBuild(
                static_cast<const core::BuildExpression&>(expression));
            break;
        case core::AstNodeKind::MemberAccessExpression:
            type = AnalyzeMemberAccess(
                static_cast<const core::MemberAccessExpression&>(expression));
            break;
        case core::AstNodeKind::MemberCallExpression:
            type = AnalyzeMemberCall(
                static_cast<const core::MemberCallExpression&>(expression));
            break;
        case core::AstNodeKind::SliceExpression:
            type = AnalyzeSlice(
                static_cast<const core::SliceExpression&>(expression),
                expected);
            break;
        case core::AstNodeKind::IndexExpression:
            type = AnalyzeIndex(
                static_cast<const core::IndexExpression&>(expression));
            break;
        case core::AstNodeKind::CastExpression:
            type = AnalyzeCast(
                static_cast<const core::CastExpression&>(expression));
            break;
        case core::AstNodeKind::IsOfExpression:
            type = AnalyzeIsOf(
                static_cast<const core::IsOfExpression&>(expression));
            break;
        default:
            break;
    }
    _model._types.insert_or_assign(&expression, type);
    if (!_directConstructions.contains(&expression) && type.OwnsStorage() &&
        (expression.Kind() == core::AstNodeKind::CallExpression ||
         expression.Kind() == core::AstNodeKind::MemberCallExpression ||
         expression.Kind() == core::AstNodeKind::BuildExpression))
        RecordCleanupEffects(type);
    return type;
}

SemanticType SemanticAnalyzer::AnalyzeIsOf(
    const core::IsOfExpression& expression) {
    SemanticType value = AnalyzeExpression(expression.Value());
    bool result = false;
    for (const auto& candidateSyntax : expression.Candidates()) {
        SemanticType candidate = ResolveType(*candidateSyntax);
        if (expression.IsTypeSetQuery()) {
            value = value.WithoutConst();
            candidate = candidate.WithoutConst();
        }
        if (value == candidate) {
            result = true;
            break;
        }
    }
    const_cast<core::IsOfExpression&>(expression).SetResult(result);
    return SemanticType(SemanticTypeKind::Bool);
}

std::unique_ptr<core::TypeNode> SemanticAnalyzer::TypeSyntax(
    SemanticType type, source::SourceRange range,
    core::OwnershipContract contract) {
    using core::PrimitiveType;
    using core::TypeKind;

    const auto primitive = [&](PrimitiveType value) {
        return std::make_unique<core::TypeNode>(range, value, type.IsConst());
    };
    const auto visibleName = [&](SymbolKind kind) {
        for (const auto& symbol : _model._symbols)
            if (symbol->Kind() == kind && symbol->Type() == type.WithoutConst())
                if (const auto name = _currentScope->NameFor(*symbol))
                    return *name;
        return type.Name();
    };
    std::unique_ptr<core::TypeNode> result;
    switch (type.Kind()) {
        case SemanticTypeKind::I8:
            result = primitive(PrimitiveType::I8);
            break;
        case SemanticTypeKind::I16:
            result = primitive(PrimitiveType::I16);
            break;
        case SemanticTypeKind::I32:
            result = primitive(PrimitiveType::I32);
            break;
        case SemanticTypeKind::I64:
            result = primitive(PrimitiveType::I64);
            break;
        case SemanticTypeKind::ISize:
            result = primitive(PrimitiveType::ISize);
            break;
        case SemanticTypeKind::U8:
            result = primitive(PrimitiveType::U8);
            break;
        case SemanticTypeKind::U16:
            result = primitive(PrimitiveType::U16);
            break;
        case SemanticTypeKind::U32:
            result = primitive(PrimitiveType::U32);
            break;
        case SemanticTypeKind::U64:
            result = primitive(PrimitiveType::U64);
            break;
        case SemanticTypeKind::USize:
            result = primitive(PrimitiveType::USize);
            break;
        case SemanticTypeKind::F32:
            result = primitive(PrimitiveType::F32);
            break;
        case SemanticTypeKind::F64:
            result = primitive(PrimitiveType::F64);
            break;
        case SemanticTypeKind::Bool:
            result = primitive(PrimitiveType::Bool);
            break;
        case SemanticTypeKind::Char:
            result = primitive(PrimitiveType::Char);
            break;
        case SemanticTypeKind::Void:
            result = primitive(PrimitiveType::Void);
            break;
        case SemanticTypeKind::String:
            result = primitive(PrimitiveType::String);
            break;
        case SemanticTypeKind::StringView:
            result = primitive(PrimitiveType::StringView);
            break;
        case SemanticTypeKind::ErrorValue:
            result = primitive(PrimitiveType::ErrorValue);
            break;
        case SemanticTypeKind::NamedError:
            result = std::make_unique<core::TypeNode>(
                range, visibleName(SymbolKind::ErrorType), type.IsConst());
            break;
        case SemanticTypeKind::Enum:
            result = std::make_unique<core::TypeNode>(
                range, visibleName(SymbolKind::EnumType), type.IsConst());
            break;
        case SemanticTypeKind::Structure:
            result = std::make_unique<core::TypeNode>(
                range, visibleName(SymbolKind::StructureType), type.IsConst());
            break;
        case SemanticTypeKind::Interface:
            result = std::make_unique<core::TypeNode>(
                range, visibleName(SymbolKind::InterfaceType), type.IsConst());
            break;
        case SemanticTypeKind::Function:
            result = std::make_unique<core::TypeNode>(
                range, visibleName(SymbolKind::CallableType), type.IsConst());
            break;
        case SemanticTypeKind::Pointer:
            result = std::make_unique<core::TypeNode>(
                range, TypeKind::Pointer, TypeSyntax(*type.Target(), range),
                type.IsConst());
            break;
        case SemanticTypeKind::OwnedBuffer: {
            auto slice = std::make_unique<core::TypeNode>(
                range, TypeKind::Slice, TypeSyntax(*type.Target(), range));
            result = std::make_unique<core::TypeNode>(
                range, TypeKind::Pointer, std::move(slice), type.IsConst());
            break;
        }
        case SemanticTypeKind::OwnedSlice:
            result = std::make_unique<core::TypeNode>(
                range, TypeKind::Slice, TypeSyntax(*type.Target(), range),
                type.IsConst());
            break;
        case SemanticTypeKind::Reference:
            result = std::make_unique<core::TypeNode>(
                range, TypeKind::Reference, TypeSyntax(*type.Target(), range),
                type.IsConst());
            break;
        case SemanticTypeKind::Array:
            result = std::make_unique<core::TypeNode>(
                range, TypeKind::Array, TypeSyntax(*type.Target(), range),
                type.IsConst(), source::SourceRange{}, type.Length());
            break;
        case SemanticTypeKind::Slice:
            result = std::make_unique<core::TypeNode>(
                range, TypeKind::Slice, TypeSyntax(*type.Target(), range),
                type.IsConst());
            break;
        case SemanticTypeKind::Tuple: {
            std::vector<std::unique_ptr<core::TypeNode>> elements;
            elements.reserve(type.Elements().size());
            for (const auto element : type.Elements())
                elements.push_back(TypeSyntax(element, range));
            result = std::make_unique<core::TypeNode>(
                range, std::move(elements), type.IsConst());
            break;
        }
        case SemanticTypeKind::Null:
        case SemanticTypeKind::None:
        case SemanticTypeKind::Error:
            result = primitive(PrimitiveType::Void);
            break;
    }
    result->SetContract(type.Kind() == SemanticTypeKind::OwnedSlice
                            ? core::OwnershipContract::Own
                            : contract);
    return result;
}

SemanticType SemanticAnalyzer::AnalyzeAnonymousFunction(
    const core::AnonymousFunctionExpression& expression,
    SemanticType expected) {
    auto& function =
        const_cast<core::FunctionDeclaration&>(expression.Function());
    if (const Symbol* existing = _model.DeclaredSymbol(function))
        return existing->Type();

    function.moduleId = _currentModule;
    function.sourceId = _currentSource;
    if (expression.HasInferredSignature()) {
        if (expected.Kind() != SemanticTypeKind::Function) {
            Report(diagnostics::DiagnosticCode::CannotInferType,
                   expression.Range(),
                   "anonymous function signature requires an expected "
                   "callable type");
            return SemanticType::Error();
        }
        const CallableSignature& signature = expected.Signature();
        if (function.Parameters().size() != signature.parameters.size()) {
            Report(diagnostics::DiagnosticCode::IncompatibleType,
                   expression.Range(),
                   "expected " + std::to_string(signature.parameters.size()) +
                       " anonymous function parameters, found " +
                       std::to_string(function.Parameters().size()));
            return SemanticType::Error();
        }
        for (std::size_t index = 0; index < signature.parameters.size();
             ++index) {
            const core::OwnershipContract contract =
                index < signature.parameterContracts.size()
                    ? signature.parameterContracts[index]
                    : core::OwnershipContract::None;
            function.Parameters()[index]->SetType(
                TypeSyntax(signature.parameters[index],
                           function.Parameters()[index]->Range(), contract));
        }

        if (signature.result.Kind() == SemanticTypeKind::Tuple) {
            std::vector<std::unique_ptr<core::TypeNode>> elements;
            elements.reserve(signature.result.Elements().size());
            for (std::size_t index = 0;
                 index < signature.result.Elements().size(); ++index) {
                const core::OwnershipContract contract =
                    index < signature.resultContracts.size()
                        ? signature.resultContracts[index]
                        : core::OwnershipContract::None;
                elements.push_back(
                    TypeSyntax(signature.result.Elements()[index],
                               expression.Range(), contract));
            }
            function.SetReturnType(std::make_unique<core::TypeNode>(
                expression.Range(), std::move(elements)));
        } else {
            const core::OwnershipContract contract =
                signature.resultContracts.empty()
                    ? core::OwnershipContract::None
                    : signature.resultContracts.front();
            function.SetReturnType(
                TypeSyntax(signature.result, expression.Range(), contract));
        }
        if (signature.isUnsafe)
            function.AddModifier(core::FunctionModifier::Unsafe);
        if (!signature.borrowSourceIndices.empty()) {
            std::vector<std::string> sources;
            std::vector<source::SourceRange> ranges;
            for (const std::size_t source : signature.borrowSourceIndices) {
                if (source >= function.Parameters().size()) continue;
                sources.push_back(function.Parameters()[source]->Name());
                ranges.push_back(function.Parameters()[source]->NameRange());
            }
            function.SetBorrowSources(std::move(sources), std::move(ranges));
        }
    }

    Scope* enclosingScope = _currentScope;
    const std::string enclosingModule = _currentModule;
    std::vector<ClosureCapture> captures;
    std::unordered_set<std::string> captureNames;
    captures.reserve(expression.Captures().size());
    for (const auto& capture : expression.Captures()) {
        if (!captureNames.insert(capture->Name()).second) {
            Report(diagnostics::DiagnosticCode::DuplicateDeclaration,
                   capture->NameRange(),
                   "duplicate capture of '" + capture->Name() + "'");
            continue;
        }
        const Symbol* source = enclosingScope->Lookup(capture->Name());
        if (source == nullptr) {
            Report(diagnostics::DiagnosticCode::UnknownName,
                   capture->NameRange(),
                   "unknown captured name '" + capture->Name() + "'");
            continue;
        }
        _model._captureSources.emplace(capture.get(), source);
        const SemanticType type = source->Type().WithoutConst();
        const bool byCopy =
            capture->Mode() ==
            core::AnonymousFunctionExpression::CaptureMode::Copy;
        const bool mutableReference =
            capture->Mode() ==
            core::AnonymousFunctionExpression::CaptureMode::Reference;
        const bool lendsResource =
            capture->Mode() ==
            core::AnonymousFunctionExpression::CaptureMode::Lend;
        const bool ownsValue =
            capture->Mode() ==
            core::AnonymousFunctionExpression::CaptureMode::Own;
        const bool rawResource = IsRawResource(type);
        const bool resourceCapture =
            lendsResource || (ownsValue && rawResource);
        if (source->Kind() != SymbolKind::Parameter &&
            source->Kind() != SymbolKind::LocalVariable) {
            Report(diagnostics::DiagnosticCode::NonCopyableValue,
                   capture->NameRange(),
                   "'" + capture->Name() + "' is not a capturable variable");
            continue;
        }
        if (byCopy && HasBorrowedResult(type)) {
            Report(diagnostics::DiagnosticCode::InvalidBorrowContract,
                   capture->NameRange(),
                   "borrowed value '" + capture->Name() +
                       "' cannot be captured by copy");
            continue;
        }
        if (resourceCapture && !function.IsUnsafe()) {
            Report(diagnostics::DiagnosticCode::UnsafeOperation,
                   capture->NameRange(),
                   std::string(lendsResource ? "lend" : "own") +
                       " capture requires an unsafe anonymous function");
            continue;
        }
        if (lendsResource && !rawResource) {
            Report(diagnostics::DiagnosticCode::NonCopyableValue,
                   capture->NameRange(),
                   "lend capture requires a raw pointer resource, found '" +
                       type.Name() + "'");
            continue;
        }
        if (ownsValue && !rawResource && type.IsCopyable()) {
            Report(diagnostics::DiagnosticCode::NonCopyableValue,
                   capture->NameRange(),
                   "value '" + capture->Name() + "' of type '" + type.Name() +
                       "' is copyable; capture it with copy instead of own");
            continue;
        }
        if (ownsValue && type.IsImmovable()) {
            Report(diagnostics::DiagnosticCode::NonCopyableValue,
                   capture->NameRange(),
                   "value '" + capture->Name() + "' of type '" + type.Name() +
                       "' is immovable and cannot be captured with own");
            continue;
        }
        if (ownsValue && !rawResource &&
            ContainsUnencapsulatedRawResource(type)) {
            Report(diagnostics::DiagnosticCode::NonCopyableValue,
                   capture->NameRange(),
                   "value '" + capture->Name() + "' of type '" + type.Name() +
                       "' contains a raw resource without safe teardown");
            continue;
        }
        if (!ownsValue && ContainsRawPointer(type) && !function.IsUnsafe()) {
            Report(diagnostics::DiagnosticCode::UnsafeOperation,
                   capture->NameRange(),
                   "raw-pointer value '" + capture->Name() +
                       "' requires an unsafe anonymous function");
            continue;
        }
        if (byCopy && !type.IsCopyable()) {
            Report(diagnostics::DiagnosticCode::NonCopyableValue,
                   capture->NameRange(),
                   "value '" + capture->Name() + "' of type '" + type.Name() +
                       "' cannot be captured by copy");
            continue;
        }
        if (mutableReference &&
            (!source->Writable() || source->Type().IsConst())) {
            Report(diagnostics::DiagnosticCode::SymbolNotWritable,
                   capture->NameRange(),
                   "cannot mutably capture read-only variable '" +
                       capture->Name() + "'");
            continue;
        }
        SemanticType capturedType = type;
        if (capture->Mode() == core::AnonymousFunctionExpression::CaptureMode::
                                   ConstReference &&
            (type.Kind() == SemanticTypeKind::Reference ||
             type.Kind() == SemanticTypeKind::Slice))
            capturedType = SemanticType(type.Kind(), type.Target()->WithConst(),
                                        type.IsConst(), type.Length());
        auto variable = std::make_unique<VariableSymbol>(
            SymbolKind::LocalVariable, capture->Name(), capturedType,
            capture->NameRange(),
            byCopy ? source->Writable() : mutableReference);
        const auto* capturedVariable = variable.get();
        _model._symbols.push_back(std::move(variable));
        _model._declarations.emplace(capture.get(), capturedVariable);
        _model._captureVariables.emplace(capturedVariable, capture.get());
        captures.push_back({capture.get(), source, capturedVariable});
    }
    RegisterFunction(function, true);
    _currentScope = enclosingScope;
    _currentModule = enclosingModule;
    const Symbol* symbol = _model.DeclaredSymbol(function);
    if (symbol == nullptr) return SemanticType::Error();
    _model._closureCaptures.emplace(&function, std::move(captures));
    if (_currentFunction && !expression.Captures().empty())
        _forbiddenZeroEffects.insert(_currentFunction);
    if (ContainsRawPointer(symbol->Type()) && !function.IsUnsafe())
        Report(diagnostics::DiagnosticCode::UnsafeOperation, expression.Range(),
               "raw pointer anonymous function signatures require 'unsafe'");
    _model._anonymousFunctions.push_back(&function);
    return symbol->Type();
}

SemanticType SemanticAnalyzer::AnalyzeMatch(
    const core::MatchExpression& expression, SemanticType expected) {
    const SemanticType valueType = AnalyzeExpression(expression.Value());
    const EnumType* enumeration =
        valueType.Kind() == SemanticTypeKind::Enum ? valueType.Enum() : nullptr;
    if (!valueType.IsError() && enumeration == nullptr)
        Report(diagnostics::DiagnosticCode::InvalidMatch,
               expression.Value().Range(),
               "match value must have an enum type, found '" +
                   valueType.Name() + "'");

    if (expression.Arms().empty())
        Report(diagnostics::DiagnosticCode::InvalidMatch, expression.Range(),
               "match expression must contain at least one arm");

    std::unordered_set<std::string> covered;
    bool wildcard = false;
    SemanticType result =
        expected.IsError() ? SemanticType::Error() : expected.WithoutConst();
    for (std::size_t index = 0; index < expression.Arms().size(); ++index) {
        const core::MatchArm& arm = *expression.Arms()[index];
        if (wildcard) {
            Report(diagnostics::DiagnosticCode::InvalidMatch,
                   arm.PatternRange(),
                   "match arm is unreachable after wildcard pattern");
        } else if (arm.IsWildcard()) {
            wildcard = true;
        } else if (enumeration != nullptr) {
            const Symbol* patternType = _currentScope->Lookup(arm.EnumName());
            if (patternType == nullptr ||
                patternType->Kind() != SymbolKind::EnumType) {
                Report(diagnostics::DiagnosticCode::InvalidMatch,
                       arm.PatternRange(),
                       "match pattern type '" + arm.EnumName() +
                           "' is not an enum");
            } else if (patternType->Type().Enum() != enumeration) {
                Report(diagnostics::DiagnosticCode::InvalidMatch,
                       arm.PatternRange(),
                       "pattern for enum '" + patternType->Type().Name() +
                           "' cannot match value of type '" + valueType.Name() +
                           "'");
            } else {
                const auto variant = std::find_if(
                    enumeration->variants.begin(), enumeration->variants.end(),
                    [&](const EnumVariant& candidate) {
                        return candidate.name == arm.Variant();
                    });
                if (variant == enumeration->variants.end()) {
                    Report(diagnostics::DiagnosticCode::InvalidMatch,
                           arm.PatternRange(),
                           "enum '" + valueType.Name() + "' has no variant '" +
                               arm.Variant() + "'");
                } else if (!covered.insert(arm.Variant()).second) {
                    Report(diagnostics::DiagnosticCode::InvalidMatch,
                           arm.PatternRange(),
                           "duplicate match pattern '" + arm.EnumName() + "." +
                               arm.Variant() + "'");
                }
            }
        }

        const SemanticType armType = AnalyzeExpression(
            arm.Value(), result.IsError() ? SemanticType::Error() : result);
        if (armType.IsError()) continue;
        if (result.IsError()) {
            result = armType.WithoutConst();
        } else if (!IsAssignable(result, armType)) {
            Report(diagnostics::DiagnosticCode::InvalidMatch,
                   arm.Value().Range(),
                   "match arm has type '" + armType.Name() + "', expected '" +
                       result.Name() + "'");
        }
    }

    if (enumeration != nullptr && !wildcard) {
        std::string missing;
        for (const EnumVariant& variant : enumeration->variants) {
            if (covered.contains(variant.name)) continue;
            if (!missing.empty()) missing += ", ";
            missing += SemanticType(enumeration).Name() + "." + variant.name;
        }
        if (!missing.empty())
            Report(diagnostics::DiagnosticCode::InvalidMatch,
                   expression.Range(),
                   "non-exhaustive match; missing " + missing);
    }
    if (!result.IsError() && result.HasBorrowedLifetime())
        Report(diagnostics::DiagnosticCode::InvalidMatch, expression.Range(),
               "match expressions cannot yet produce borrowed values");
    return result;
}

void SemanticAnalyzer::ValidateArrayType(SemanticType type,
                                         source::SourceRange range) {
    if (!IsArrayElement(*type.Target())) {
        Report(diagnostics::DiagnosticCode::InvalidArrayElement, range,
               "array elements must be unqualified scalars, error values, raw "
               "pointers, or "
               "structures, or arrays of those types; use const [N]T for a "
               "read-only array");
    } else if (!StorageSize(type)) {
        Report(diagnostics::DiagnosticCode::InvalidArrayLength, range,
               "array layout exceeds the supported native size");
    }
}

SemanticType SemanticAnalyzer::AnalyzeArray(
    const core::ArrayExpression& expression, SemanticType expected) {
    SemanticType element = expected.Kind() == SemanticTypeKind::Array
                               ? *expected.Target()
                               : SemanticType::Error();
    if (expected.Kind() == SemanticTypeKind::Array &&
        expression.Length() != expected.Length())
        Report(diagnostics::DiagnosticCode::ArrayInitializerCount,
               expression.Range(),
               "expected " + std::to_string(expected.Length()) +
                   " array elements, found " +
                   std::to_string(expression.Length()));
    for (const std::unique_ptr<core::Expression>& item :
         expression.Elements()) {
        const SemanticType actual = AnalyzeExpression(*item, element);
        if (element.IsError())
            element = actual.WithoutConst();
        else if (!IsAssignable(element, actual))
            Report(diagnostics::DiagnosticCode::IncompatibleType, item->Range(),
                   TypeMismatch(element, actual));
    }
    if (element.IsError()) {
        Report(diagnostics::DiagnosticCode::CannotInferType, expression.Range(),
               "empty array initializer requires an expected array type");
        return SemanticType::Error();
    }
    const SemanticType result(SemanticTypeKind::Array, element, false,
                              expression.Length());
    ValidateArrayType(result, expression.Range());
    return result;
}

SemanticType SemanticAnalyzer::AnalyzeLiteral(
    const core::LiteralExpression& expression, SemanticType expected,
    bool negative) {
    if (expression.LiteralType() == TokenType::NumberLiteral &&
        expected.IsInteger()) {
        const SemanticTypeKind kind = expected.Kind();
        const bool isSigned = kind <= SemanticTypeKind::ISize;
        unsigned bits = 64;
        if (kind == SemanticTypeKind::I8 || kind == SemanticTypeKind::U8)
            bits = 8;
        if (kind == SemanticTypeKind::I16 || kind == SemanticTypeKind::U16)
            bits = 16;
        if (kind == SemanticTypeKind::I32 || kind == SemanticTypeKind::U32)
            bits = 32;
        if (kind == SemanticTypeKind::ISize || kind == SemanticTypeKind::USize)
            bits = sizeof(void*) * 8;
        std::uint64_t limit = std::numeric_limits<std::uint64_t>::max();
        if (isSigned)
            limit = (std::uint64_t{1} << (bits - 1)) - (negative ? 0 : 1);
        else if (bits < 64)
            limit = (std::uint64_t{1} << bits) - 1;
        std::uint64_t value = 0;
        const std::string& text = expression.Value();
        if (!core::ParseUnsignedIntegerLiteral(text, value) || value > limit ||
            (negative && !isSigned && value != 0)) {
            Report(diagnostics::DiagnosticCode::IntegerLiteralOutOfRange,
                   expression.Range(),
                   "integer literal is not representable as '" +
                       expected.Name() + "'");
        }
        return expected.WithoutConst();
    }
    switch (expression.LiteralType()) {
        case TokenType::NumberLiteral:
            return SemanticType(SemanticTypeKind::I32);
        case TokenType::FloatLiteral:
            return SemanticType(SemanticTypeKind::F64);
        case TokenType::True:
        case TokenType::False:
            return SemanticType(SemanticTypeKind::Bool);
        case TokenType::CharLiteral:
            return SemanticType(SemanticTypeKind::Char);
        case TokenType::StringLiteral:
            return SemanticType(SemanticTypeKind::StringView);
        case TokenType::Null:
            return SemanticType(SemanticTypeKind::Null);
        case TokenType::None:
            if ((expected.ContainsStructure() ||
                 expected.Kind() == SemanticTypeKind::Enum) &&
                expected.IsZeroable()) {
                if (expected.IsImmovable()) RequireUnsafe(expression.Range());
                WarnForNontrivialDefault(expected, expression.Range());
                RecordZeroDependency(expected);
                if (!_directConstructions.contains(&expression))
                    RecordCleanupEffects(expected);
            }
            return SemanticType(SemanticTypeKind::None);
        default:
            return SemanticType::Error();
    }
}

void SemanticAnalyzer::RecordZeroDependency(SemanticType type) {
    if (type.Kind() == SemanticTypeKind::Array) {
        RecordZeroDependency(*type.Target());
        return;
    }
    if (type.Kind() == SemanticTypeKind::Tuple) {
        for (const auto& element : type.Elements())
            RecordZeroDependency(element);
        return;
    }
    if (type.Kind() != SemanticTypeKind::Structure) return;
    if (_currentFunction && type.Structure()->zero)
        _functionCalls[_currentFunction].insert(type.Structure()->zero);
    if (_inZero && _currentFunction && _currentFunction->Owner())
        _zeroDependencies[_currentFunction->Owner()].insert(type.Structure());
}

void SemanticAnalyzer::RecordCleanupEffects(SemanticType type) {
    if (!_currentFunction) return;
    if (type.Kind() == SemanticTypeKind::Array) {
        RecordCleanupEffects(*type.Target());
        return;
    }
    if (type.Kind() == SemanticTypeKind::Tuple) {
        for (const auto& element : type.Elements())
            RecordCleanupEffects(element);
        return;
    }
    if (type.Kind() != SemanticTypeKind::Structure) return;
    if (type.Structure()->tear)
        _functionCalls[_currentFunction].insert(type.Structure()->tear);
    for (const auto& field : type.Structure()->fields)
        if (field.type.OwnsStorage()) RecordCleanupEffects(field.type);
}

void SemanticAnalyzer::ValidateZeroCycles() {
    enum class Visit { Unseen, Active, Complete };
    std::unordered_map<const StructureType*, Visit> visits;
    std::unordered_set<const StructureType*> reported;
    std::function<void(const StructureType*)> visit =
        [&](const StructureType* structure) {
            visits[structure] = Visit::Active;
            for (const StructureType* dependency :
                 _zeroDependencies[structure]) {
                if (visits[dependency] == Visit::Active) {
                    if (reported.insert(structure).second)
                        Report(diagnostics::DiagnosticCode::InvalidLifecycle,
                               structure->syntax->NameRange(),
                               "recursive default construction involving '" +
                                   structure->name +
                                   "::zero()' is not allowed");
                    continue;
                }
                if (visits[dependency] == Visit::Unseen) visit(dependency);
            }
            visits[structure] = Visit::Complete;
        };
    for (const auto& structure : _model._structures)
        if (visits[structure.get()] == Visit::Unseen) visit(structure.get());
}

void SemanticAnalyzer::ValidateZeroEffects() {
    const auto hasMutableBorrow = [](const FunctionSymbol* function) {
        for (SemanticType parameter : function->ParameterTypes())
            if ((parameter.Kind() == SemanticTypeKind::Reference ||
                 parameter.Kind() == SemanticTypeKind::Pointer) &&
                !parameter.Target()->IsConst())
                return true;
        return false;
    };
    for (const auto& structure : _model._structures) {
        const FunctionSymbol* zero = structure->zero;
        if (!zero || structure->zeroIsGenerated) continue;
        std::unordered_set<const FunctionSymbol*> visiting;
        std::function<bool(const FunctionSymbol*)> invalid =
            [&](const FunctionSymbol* function) {
                if (!visiting.insert(function).second) return function == zero;
                if (function->IsUnsafe() ||
                    _forbiddenZeroEffects.contains(function) ||
                    (function != zero &&
                     !function->Name().ends_with("::tear") &&
                     hasMutableBorrow(function)))
                    return true;
                for (const FunctionSymbol* callee : _functionCalls[function])
                    if (invalid(callee)) return true;
                return false;
            };
        if (invalid(zero))
            Report(diagnostics::DiagnosticCode::InvalidLifecycle,
                   structure->syntax->NameRange(),
                   "zero must be non-recursive and cannot allocate, use "
                   "unsafe operations, or cause external side effects, "
                   "including through helper calls");
    }
}

void SemanticAnalyzer::ClassifyTrivialZeros(const core::CompilationUnit& unit) {
    std::unordered_map<const FunctionSymbol*, const core::FunctionDeclaration*>
        declarations;
    const auto remember = [&](const core::FunctionDeclaration& declaration) {
        const Symbol* symbol = _model.DeclaredSymbol(declaration);
        if (symbol && symbol->Kind() == SymbolKind::Function)
            declarations.emplace(static_cast<const FunctionSymbol*>(symbol),
                                 &declaration);
    };
    for (const auto& function : _model._generatedFunctions) remember(*function);
    for (const auto& declaration : unit.Declarations())
        if (declaration->Kind() == core::AstNodeKind::FunctionDeclaration)
            remember(
                static_cast<const core::FunctionDeclaration&>(*declaration));

    enum class Visit { Unseen, Active, Complete };
    std::unordered_map<const StructureType*, Visit> visits;
    std::unordered_map<const StructureType*, bool> trivialities;
    std::function<bool(SemanticType)> trivialType;
    std::function<bool(const StructureType*)> trivialStructure;
    std::function<bool(const core::Expression&, SemanticType)> trivialValue;
    std::function<bool(const core::BuildExpression&, const StructureType*)>
        trivialBuild;

    trivialType = [&](SemanticType type) {
        if (type.Kind() == SemanticTypeKind::Enum)
            return type.Enum()->defaultValue.has_value() &&
                   *type.Enum()->defaultValue == 0;
        if (type.Kind() == SemanticTypeKind::Structure)
            return trivialStructure(type.Structure());
        if (type.Kind() == SemanticTypeKind::Array)
            return trivialType(*type.Target());
        if (type.Kind() == SemanticTypeKind::Tuple) {
            for (const SemanticType element : type.Elements())
                if (!trivialType(element)) return false;
            return true;
        }
        return type.IsNumeric() || type.Kind() == SemanticTypeKind::Bool ||
               type.Kind() == SemanticTypeKind::Char ||
               type.Kind() == SemanticTypeKind::String ||
               type.Kind() == SemanticTypeKind::Pointer ||
               type.Kind() == SemanticTypeKind::OwnedBuffer ||
               type.Kind() == SemanticTypeKind::OwnedSlice;
    };

    trivialValue = [&](const core::Expression& expression,
                       SemanticType expected) -> bool {
        if (expression.Kind() == core::AstNodeKind::ParenthesizedExpression)
            return trivialValue(
                static_cast<const core::ParenthesizedExpression&>(expression)
                    .Inner(),
                expected);
        if (expression.Kind() == core::AstNodeKind::LiteralExpression) {
            const auto& literal =
                static_cast<const core::LiteralExpression&>(expression);
            switch (literal.LiteralType()) {
                case TokenType::NumberLiteral:
                    return expected.IsNumeric() &&
                           core::IsZeroIntegerLiteral(literal.Value());
                case TokenType::FloatLiteral:
                    return expected.IsNumeric() &&
                           std::all_of(literal.Value().begin(),
                                       literal.Value().end(), [](char value) {
                                           return value == '0' || value == '.';
                                       });
                case TokenType::False:
                    return expected.Kind() == SemanticTypeKind::Bool;
                case TokenType::CharLiteral:
                    return expected.Kind() == SemanticTypeKind::Char &&
                           literal.Value() == std::string(1, '\0');
                case TokenType::Null:
                    return expected.Kind() == SemanticTypeKind::Pointer ||
                           expected.Kind() == SemanticTypeKind::OwnedBuffer;
                case TokenType::None:
                    return trivialType(expected);
                default:
                    return false;
            }
        }
        if (expression.Kind() == core::AstNodeKind::UnaryExpression) {
            const auto& unary =
                static_cast<const core::UnaryExpression&>(expression);
            return expected.IsNumeric() &&
                   (unary.OperatorType() == TokenType::Add ||
                    unary.OperatorType() == TokenType::Sub) &&
                   trivialValue(unary.Operand(), expected);
        }
        if (expression.Kind() == core::AstNodeKind::CastExpression) {
            const auto& cast =
                static_cast<const core::CastExpression&>(expression);
            const SemanticType source = _model.TypeOf(cast.Value());
            return !source.IsError() && trivialValue(cast.Value(), source);
        }
        if (expression.Kind() == core::AstNodeKind::TupleExpression &&
            expected.Kind() == SemanticTypeKind::Tuple) {
            const auto& tuple =
                static_cast<const core::TupleExpression&>(expression);
            if (tuple.Elements().size() != expected.Elements().size())
                return false;
            for (std::size_t index = 0; index < tuple.Elements().size();
                 ++index)
                if (!trivialValue(*tuple.Elements()[index],
                                  expected.Elements()[index]))
                    return false;
            return true;
        }
        if (expression.Kind() == core::AstNodeKind::ArrayExpression &&
            expected.Kind() == SemanticTypeKind::Array) {
            const auto& array =
                static_cast<const core::ArrayExpression&>(expression);
            for (const auto& element : array.Elements())
                if (!trivialValue(*element, *expected.Target())) return false;
            return true;
        }
        if (expression.Kind() == core::AstNodeKind::BuildExpression &&
            expected.Kind() == SemanticTypeKind::Structure) {
            const auto& build =
                static_cast<const core::BuildExpression&>(expression);
            const SemanticType actual = _model.TypeOf(build);
            return actual.Kind() == SemanticTypeKind::Structure &&
                   actual.Structure() == expected.Structure() &&
                   trivialBuild(build, expected.Structure());
        }
        if (expression.Kind() == core::AstNodeKind::MemberCallExpression &&
            expected.Kind() == SemanticTypeKind::Structure) {
            const auto& call =
                static_cast<const core::MemberCallExpression&>(expression);
            return call.Arguments().empty() &&
                   _model.MethodFor(call) == expected.Structure()->zero &&
                   trivialStructure(expected.Structure());
        }
        if (expression.Kind() == core::AstNodeKind::MemberAccessExpression &&
            expected.Kind() == SemanticTypeKind::Enum) {
            const auto& access =
                static_cast<const core::MemberAccessExpression&>(expression);
            if (_model.TypeOf(access).Enum() != expected.Enum()) return false;
            for (const EnumVariant& variant : expected.Enum()->variants)
                if (variant.name == access.Member()) return variant.value == 0;
        }
        return false;
    };

    trivialBuild = [&](const core::BuildExpression& build,
                       const StructureType* structure) {
        std::unordered_set<const StructureField*> initialized;
        for (const auto& statement : build.Body().Statements()) {
            if (statement->Kind() != core::AstNodeKind::ExpressionStatement)
                return false;
            const auto& value =
                static_cast<const core::ExpressionStatement&>(*statement)
                    .Value();
            if (value.Kind() != core::AstNodeKind::AssignmentExpression)
                return false;
            const auto& assignment =
                static_cast<const core::AssignmentExpression&>(value);
            if (assignment.Target().Kind() !=
                core::AstNodeKind::MemberAccessExpression)
                return false;
            const auto& target =
                static_cast<const core::MemberAccessExpression&>(
                    assignment.Target());
            if (target.Receiver().Kind() != core::AstNodeKind::NameExpression ||
                static_cast<const core::NameExpression&>(target.Receiver())
                        .Name() != "self")
                return false;
            const StructureField* field = _model.FieldFor(target);
            if (!field || !trivialValue(assignment.Value(), field->type))
                return false;
            initialized.insert(field);
        }
        for (const StructureField& field : structure->fields)
            if (!initialized.contains(&field) && !trivialType(field.type))
                return false;
        return true;
    };

    trivialStructure = [&](const StructureType* structure) {
        if (!structure->zero) return false;
        if (visits[structure] == Visit::Complete)
            return trivialities[structure];
        if (visits[structure] == Visit::Active) return false;
        visits[structure] = Visit::Active;
        bool result = false;
        if (structure->zeroIsGenerated) {
            result = true;
            for (const StructureField& field : structure->fields)
                if (!trivialType(field.type)) {
                    result = false;
                    break;
                }
        } else if (const auto found = declarations.find(structure->zero);
                   found != declarations.end()) {
            const auto& statements = found->second->Body().Statements();
            if (statements.size() == 1 &&
                statements.front()->Kind() ==
                    core::AstNodeKind::ReturnStatement) {
                const core::Expression* value =
                    static_cast<const core::ReturnStatement&>(
                        *statements.front())
                        .Value();
                while (value && value->Kind() ==
                                    core::AstNodeKind::ParenthesizedExpression)
                    value = &static_cast<const core::ParenthesizedExpression&>(
                                 *value)
                                 .Inner();
                if (value &&
                    value->Kind() == core::AstNodeKind::BuildExpression)
                    result = trivialBuild(
                        static_cast<const core::BuildExpression&>(*value),
                        structure);
            }
        }
        trivialities[structure] = result;
        visits[structure] = Visit::Complete;
        return result;
    };

    for (const auto& structure : _model._structures)
        structure->zeroIsTrivial = trivialStructure(structure.get());
}

void SemanticAnalyzer::WarnForNontrivialDefault(SemanticType type,
                                                source::SourceRange range) {
    if (!_zeroTrivialityKnown) return;
    if (type.Kind() == SemanticTypeKind::Array) {
        WarnForNontrivialDefault(*type.Target(), range);
        return;
    }
    if (type.Kind() == SemanticTypeKind::Tuple) {
        for (const auto& element : type.Elements())
            WarnForNontrivialDefault(element, range);
        return;
    }
    if (type.Kind() != SemanticTypeKind::Structure ||
        type.IsTriviallyZeroable())
        return;
    if (!_entryModules.empty() &&
        !_entryModules.contains(type.Structure()->syntax->moduleId))
        return;
    if (_warnedDefaults.insert(type.Structure()).second)
        _diagnostics.Report(
            diagnostics::DiagnosticCode::InvalidLifecycle, range,
            "structure '" + type.Name() +
                "' is not trivially zeroed; default construction uses '" +
                type.Name() + "::zero()'",
            diagnostics::DiagnosticSeverity::Warning);
    for (const auto& field : type.Structure()->fields)
        WarnForNontrivialDefault(field.type, range);
}

SemanticType SemanticAnalyzer::AnalyzeCast(
    const core::CastExpression& expression) {
    const SemanticType source = AnalyzeExpression(expression.Value());
    const SemanticType target = ResolveType(expression.TargetType());
    if (source.IsError() || target.IsError()) return SemanticType::Error();
    if (!CanExplicitlyCast(source, target)) {
        Report(diagnostics::DiagnosticCode::InvalidCast, expression.Range(),
               "cannot cast '" + std::string(source.Name()) + "' to '" +
                   std::string(target.Name()) + "'");
        return SemanticType::Error();
    }
    if (target.Kind() == SemanticTypeKind::Reference &&
        target.Target()->Kind() == SemanticTypeKind::Interface &&
        (source.Kind() == SemanticTypeKind::Structure ||
         source.Kind() == SemanticTypeKind::Enum) &&
        !IsSafePlace(expression.Value())) {
        Report(diagnostics::DiagnosticCode::InvalidCast, expression.Range(),
               "casting concrete storage to an interface reference requires "
               "an addressable value");
        return SemanticType::Error();
    }
    if (_currentFunction && target.Kind() == SemanticTypeKind::String &&
        source.Kind() != SemanticTypeKind::String)
        _forbiddenZeroEffects.insert(_currentFunction);
    return target.WithoutConst();
}

SemanticType SemanticAnalyzer::AnalyzeName(
    const core::NameExpression& expression) {
    const std::string lookupName = EffectName(
        expression.Name(), _analyzingCallCallee && _callCalleeCrashable);
    if (ReportAmbiguousImport(lookupName, expression.Range()))
        return SemanticType::Error();
    const Symbol* symbol = _currentScope->Lookup(lookupName);
    if (symbol == nullptr) {
        if (!ReportInaccessibleImport(lookupName, expression.Range()))
            Report(diagnostics::DiagnosticCode::UnknownName, expression.Range(),
                   "unknown name '" + lookupName + "'");
        return SemanticType::Error();
    }
    if (!_analyzingCallCallee && symbol->Kind() == SymbolKind::Function &&
        LookupFunctions(lookupName).size() > 1) {
        Report(diagnostics::DiagnosticCode::InvalidOperand, expression.Range(),
               "overloaded function '" + expression.Name() +
                   "' must be selected by a call");
        return SemanticType::Error();
    }
    if (symbol == _buildSelf && !_allowBuildSelf)
        Report(
            diagnostics::DiagnosticCode::InvalidStructure, expression.Range(),
            "an unfinished build value cannot escape or receive method calls");
    if (symbol->Kind() == SymbolKind::ErrorType ||
        symbol->Kind() == SymbolKind::StructureType ||
        symbol->Kind() == SymbolKind::EnumType ||
        symbol->Kind() == SymbolKind::CallableType) {
        Report(diagnostics::DiagnosticCode::InvalidOperand, expression.Range(),
               "type '" + expression.Name() + "' is not a value");
        return SemanticType::Error();
    }
    if (symbol->Kind() == SymbolKind::Function &&
        static_cast<const FunctionSymbol*>(symbol)->BuiltIn() !=
            BuiltInFunction::None &&
        !_analyzingCallCallee) {
        Report(diagnostics::DiagnosticCode::InvalidOperand, expression.Range(),
               "built-in function '" + expression.Name() +
                   "' cannot be stored as a callable value");
        return SemanticType::Error();
    }
    if (symbol->Kind() == SymbolKind::Function &&
        static_cast<const FunctionSymbol*>(symbol)->Declaration() != nullptr &&
        static_cast<const FunctionSymbol*>(symbol)
            ->Declaration()
            ->HasVariadicPack() &&
        !_analyzingCallCallee) {
        Report(diagnostics::DiagnosticCode::InvalidVariadic, expression.Range(),
               "variadic forwarding function '" + expression.Name() +
                   "' cannot be stored as a callable value");
        return SemanticType::Error();
    }
    _model._bindings.emplace(&expression, symbol);
    return symbol->Type().WithoutConst();
}

SemanticType SemanticAnalyzer::AnalyzeMemberAccess(
    const core::MemberAccessExpression& expression) {
    const Symbol* symbol = nullptr;
    if (expression.Receiver().Kind() == core::AstNodeKind::NameExpression) {
        const auto& receiver =
            static_cast<const core::NameExpression&>(expression.Receiver());
        if (const auto primitive = PrimitiveNumericType(receiver.Name())) {
            if (expression.Access() != TokenType::Dot)
                Report(diagnostics::DiagnosticCode::InvalidMember,
                       expression.Range(), "numeric type properties use '.'");
            const bool integerProperty =
                primitive->IsInteger() && (expression.Member() == "min_value" ||
                                           expression.Member() == "max_value");
            const bool floatingProperty =
                primitive->IsFloating() &&
                IsFloatingConstant(expression.Member());
            if (!integerProperty && !floatingProperty) {
                Report(diagnostics::DiagnosticCode::InvalidMember,
                       expression.MemberRange(),
                       "type '" + std::string(primitive->Name()) +
                           "' has no type property '" + expression.Member() +
                           "'");
                return SemanticType::Error();
            }
            return *primitive;
        }
        symbol = _currentScope->Lookup(receiver.Name());
    }
    if (symbol && symbol->Kind() == SymbolKind::EnumType) {
        if (expression.Access() != TokenType::Dot)
            Report(diagnostics::DiagnosticCode::InvalidMember,
                   expression.Range(), "enum variants use '.'");
        const auto& enumeration = *symbol->Type().Enum();
        const auto variant = std::find_if(
            enumeration.variants.begin(), enumeration.variants.end(),
            [&](const EnumVariant& candidate) {
                return candidate.name == expression.Member();
            });
        if (variant == enumeration.variants.end()) {
            Report(diagnostics::DiagnosticCode::InvalidMember,
                   expression.MemberRange(),
                   "enum '" + symbol->Type().Name() + "' has no variant '" +
                       expression.Member() + "'");
            return SemanticType::Error();
        }
        return symbol->Type();
    }
    if (!symbol || symbol->Kind() != SymbolKind::ErrorType) {
        const bool previousAllow = _allowBuildSelf;
        const core::Expression* base = &expression.Receiver();
        while (base->Kind() == core::AstNodeKind::ParenthesizedExpression)
            base = &static_cast<const core::ParenthesizedExpression&>(*base)
                        .Inner();
        // Only direct field access may name unfinished self. An enclosing
        // field selection must not grant permission to copy(self).field.
        _allowBuildSelf = base->Kind() == core::AstNodeKind::NameExpression;
        SemanticType receiver = AnalyzeExpression(expression.Receiver());
        _allowBuildSelf = previousAllow;
        SemanticType storage = PlaceType(expression.Receiver());
        if (expression.Access() == TokenType::Arrow) {
            if (receiver.Kind() != SemanticTypeKind::Reference &&
                receiver.Kind() != SemanticTypeKind::Pointer) {
                Report(diagnostics::DiagnosticCode::InvalidMember,
                       expression.Range(),
                       "'->' requires a pointer or reference");
                return SemanticType::Error();
            }
            if (receiver.Kind() == SemanticTypeKind::Pointer)
                RequireUnsafe(expression.Range());
            storage = *receiver.Target();
            receiver = storage;
        } else if (expression.Access() != TokenType::Dot ||
                   receiver.Kind() != SemanticTypeKind::Structure) {
            Report(
                diagnostics::DiagnosticCode::InvalidMember, expression.Range(),
                "structure values use '.', pointers and references use '->'");
            return SemanticType::Error();
        }
        if (receiver.Kind() != SemanticTypeKind::Structure) {
            Report(diagnostics::DiagnosticCode::InvalidMember,
                   expression.Range(), "field access requires a structure");
            return SemanticType::Error();
        }
        for (const auto& field : receiver.Structure()->fields)
            if (field.name == expression.Member()) {
                if (field.syntax->IsPrivate() &&
                    CurrentOwner() != receiver.Structure())
                    Report(diagnostics::DiagnosticCode::PrivateAccess,
                           expression.MemberRange(),
                           "field '" + field.name + "' is private");
                _model._fields[&expression] = &field;
                return field.type.WithoutConst();
            }
        Report(diagnostics::DiagnosticCode::InvalidMember,
               expression.MemberRange(),
               "unknown field '" + expression.Member() + "'");
        return SemanticType::Error();
    }
    if (expression.Access() != TokenType::Dot)
        Report(diagnostics::DiagnosticCode::InvalidMember, expression.Range(),
               "error variants use '.'");
    if (symbol == nullptr || symbol->Kind() != SymbolKind::ErrorType) {
        Report(diagnostics::DiagnosticCode::InvalidMember,
               expression.Receiver().Range(),
               "error variants must be qualified by an error type name");
        return SemanticType::Error();
    }
    const auto& variants = symbol->Type().Family().variants;
    if (std::find(variants.begin(), variants.end(), expression.Member()) ==
        variants.end()) {
        Report(diagnostics::DiagnosticCode::InvalidMember,
               expression.MemberRange(),
               "error type '" + symbol->Type().Name() + "' has no variant '" +
                   expression.Member() + "'");
        return SemanticType::Error();
    }
    return symbol->Type();
}

void SemanticAnalyzer::RequireUnsafe(source::SourceRange range) {
    if (!_inUnsafe) {
        Report(diagnostics::DiagnosticCode::UnsafeOperation, range,
               "operation requires an unsafe function or unsafe block");
    }
}

bool SemanticAnalyzer::IsSafePlace(const core::Expression& expression) const {
    if (expression.Kind() == core::AstNodeKind::MemberAccessExpression) {
        const auto& field =
            static_cast<const core::MemberAccessExpression&>(expression);
        return _model.FieldFor(field) &&
               (field.Access() == TokenType::Arrow
                    ? _model.TypeOf(field.Receiver()).Kind() ==
                          SemanticTypeKind::Reference
                    : IsSafePlace(field.Receiver()));
    }
    if (expression.Kind() == core::AstNodeKind::MemberCallExpression) {
        const auto& call =
            static_cast<const core::MemberCallExpression&>(expression);
        return !PlaceType(expression).IsError() &&
               (_model.TypeOf(call.Receiver()).Kind() ==
                    SemanticTypeKind::Slice ||
                IsSafePlace(call.Receiver()));
    }
    if (expression.Kind() == core::AstNodeKind::ParenthesizedExpression)
        return IsSafePlace(
            static_cast<const core::ParenthesizedExpression&>(expression)
                .Inner());
    if (expression.Kind() == core::AstNodeKind::NameExpression)
        return !PlaceType(expression).IsError();
    if (expression.Kind() == core::AstNodeKind::IndexExpression &&
        _model.TypeOf(static_cast<const core::IndexExpression&>(expression)
                          .Receiver())
                .Kind() == SemanticTypeKind::Pointer)
        return false;
    if (expression.Kind() == core::AstNodeKind::IndexExpression)
        return !PlaceType(expression).IsError() &&
               (_model.TypeOf(
                          static_cast<const core::IndexExpression&>(expression)
                              .Receiver())
                        .Kind() == SemanticTypeKind::Slice ||
                IsSafePlace(
                    static_cast<const core::IndexExpression&>(expression)
                        .Receiver()));
    if (expression.Kind() == core::AstNodeKind::UnaryExpression) {
        const auto& unary =
            static_cast<const core::UnaryExpression&>(expression);
        return unary.OperatorType() == TokenType::Mul &&
               _model.TypeOf(unary.Operand()).Kind() ==
                   SemanticTypeKind::Reference;
    }
    return false;
}

bool SemanticAnalyzer::IsReferencePlace(
    const core::Expression& expression) const {
    if (expression.Kind() == core::AstNodeKind::MemberAccessExpression) {
        const auto& field =
            static_cast<const core::MemberAccessExpression&>(expression);
        return field.Access() == TokenType::Arrow
                   ? _model.TypeOf(field.Receiver()).Kind() ==
                         SemanticTypeKind::Reference
                   : IsReferencePlace(field.Receiver());
    }
    if (expression.Kind() == core::AstNodeKind::MemberCallExpression) {
        const auto& call =
            static_cast<const core::MemberCallExpression&>(expression);
        return _model.TypeOf(call.Receiver()).Kind() ==
                   SemanticTypeKind::Slice ||
               IsReferencePlace(call.Receiver());
    }
    if (expression.Kind() == core::AstNodeKind::ParenthesizedExpression)
        return IsReferencePlace(
            static_cast<const core::ParenthesizedExpression&>(expression)
                .Inner());
    if (expression.Kind() == core::AstNodeKind::IndexExpression &&
        _model.TypeOf(static_cast<const core::IndexExpression&>(expression)
                          .Receiver())
                .Kind() == SemanticTypeKind::Pointer)
        return false;
    if (expression.Kind() == core::AstNodeKind::IndexExpression)
        return _model.TypeOf(
                         static_cast<const core::IndexExpression&>(expression)
                             .Receiver())
                       .Kind() == SemanticTypeKind::Slice ||
               IsReferencePlace(
                   static_cast<const core::IndexExpression&>(expression)
                       .Receiver());
    if (expression.Kind() == core::AstNodeKind::UnaryExpression) {
        const auto& unary =
            static_cast<const core::UnaryExpression&>(expression);
        return unary.OperatorType() == TokenType::Mul &&
               _model.TypeOf(unary.Operand()).Kind() ==
                   SemanticTypeKind::Reference;
    }
    return false;
}

SemanticType SemanticAnalyzer::PlaceType(
    const core::Expression& expression) const {
    if (expression.Kind() == core::AstNodeKind::MemberAccessExpression) {
        const auto& field =
            static_cast<const core::MemberAccessExpression&>(expression);
        const StructureField* resolved = _model.FieldFor(field);
        if (!resolved) return SemanticType::Error();
        SemanticType receiver = field.Access() == TokenType::Arrow
                                    ? *_model.TypeOf(field.Receiver()).Target()
                                    : PlaceType(field.Receiver());
        if (receiver.IsError()) return receiver;
        return receiver.IsConst() ? resolved->type.WithConst() : resolved->type;
    }
    if (expression.Kind() == core::AstNodeKind::MemberCallExpression) {
        const auto& call =
            static_cast<const core::MemberCallExpression&>(expression);
        if (call.Member() != "front" && call.Member() != "back")
            return SemanticType::Error();
        const SemanticType value = _model.TypeOf(call.Receiver());
        if (value.Kind() == SemanticTypeKind::Slice ||
            value.Kind() == SemanticTypeKind::Pointer ||
            value.Kind() == SemanticTypeKind::OwnedBuffer ||
            value.Kind() == SemanticTypeKind::OwnedSlice) {
            SemanticType element = *value.Target();
            const SemanticType receiver = PlaceType(call.Receiver());
            const core::Expression* source = &call.Receiver();
            while (source->Kind() == core::AstNodeKind::ParenthesizedExpression)
                source =
                    &static_cast<const core::ParenthesizedExpression&>(*source)
                         .Inner();
            if (source->Kind() != core::AstNodeKind::NameExpression &&
                receiver.IsConst())
                element = element.WithConst();
            return element;
        }
        const SemanticType place = PlaceType(call.Receiver());
        if (place.Kind() == SemanticTypeKind::Array)
            return place.IsConst() ? place.Target()->WithConst()
                                   : *place.Target();
        return SemanticType::Error();
    }
    if (expression.Kind() == core::AstNodeKind::ParenthesizedExpression) {
        return PlaceType(
            static_cast<const core::ParenthesizedExpression&>(expression)
                .Inner());
    }
    if (expression.Kind() == core::AstNodeKind::NameExpression) {
        const Symbol* symbol = _model.SymbolFor(
            static_cast<const core::NameExpression&>(expression));
        return symbol == nullptr || symbol->Kind() == SymbolKind::ModuleConstant
                   ? SemanticType::Error()
                   : symbol->Type();
    }
    if (expression.Kind() == core::AstNodeKind::IndexExpression) {
        const auto& index =
            static_cast<const core::IndexExpression&>(expression);
        const SemanticType value = _model.TypeOf(index.Receiver());
        if (value.Kind() == SemanticTypeKind::Slice ||
            value.Kind() == SemanticTypeKind::Pointer ||
            value.Kind() == SemanticTypeKind::OwnedBuffer ||
            value.Kind() == SemanticTypeKind::OwnedSlice) {
            SemanticType element = *value.Target();
            const SemanticType receiver = PlaceType(index.Receiver());
            const core::Expression* source = &index.Receiver();
            while (source->Kind() == core::AstNodeKind::ParenthesizedExpression)
                source =
                    &static_cast<const core::ParenthesizedExpression&>(*source)
                         .Inner();
            if (source->Kind() != core::AstNodeKind::NameExpression &&
                receiver.IsConst())
                element = element.WithConst();
            return element;
        }
        const SemanticType receiver = PlaceType(index.Receiver());
        if (receiver.Kind() == SemanticTypeKind::Array)
            return receiver.IsConst() ? receiver.Target()->WithConst()
                                      : *receiver.Target();
        return SemanticType::Error();
    }
    if (expression.Kind() == core::AstNodeKind::UnaryExpression) {
        const auto& unary =
            static_cast<const core::UnaryExpression&>(expression);
        const SemanticType pointer = _model.TypeOf(unary.Operand());
        if (unary.OperatorType() == TokenType::Mul &&
            (pointer.Kind() == SemanticTypeKind::Pointer ||
             pointer.Kind() == SemanticTypeKind::Reference))
            return *pointer.Target();
    }
    return SemanticType::Error();
}

SemanticType SemanticAnalyzer::AnalyzeUnary(
    const core::UnaryExpression& expression, SemanticType expected) {
    const TokenType operation = expression.OperatorType();
    const bool expectsInteger = operation == TokenType::Sub ||
                                operation == TokenType::Add ||
                                operation == TokenType::BitNot;
    SemanticType operand = SemanticType::Error();
    const core::Expression* literal = &expression.Operand();
    while (literal->Kind() == core::AstNodeKind::ParenthesizedExpression)
        literal = &static_cast<const core::ParenthesizedExpression&>(*literal)
                       .Inner();
    if ((operation == TokenType::Sub || operation == TokenType::Add) &&
        expected.IsInteger() &&
        literal->Kind() == core::AstNodeKind::LiteralExpression) {
        operand = AnalyzeLiteral(
            static_cast<const core::LiteralExpression&>(*literal), expected,
            operation == TokenType::Sub);
        const core::Expression* layer = &expression.Operand();
        for (;;) {
            _model._types.insert_or_assign(layer, operand);
            if (layer == literal) break;
            layer = &static_cast<const core::ParenthesizedExpression&>(*layer)
                         .Inner();
        }
    } else {
        operand = AnalyzeExpression(
            expression.Operand(),
            expectsInteger ? expected : SemanticType::Error());
    }
    if (operand.IsError()) return operand;
    if (expression.IsReferenceDeref() &&
        operand.Kind() != SemanticTypeKind::Reference) {
        Report(diagnostics::DiagnosticCode::InvalidReference,
               expression.Range(),
               "deref() requires a checked reference receiver");
        return SemanticType::Error();
    }
    if (operation == TokenType::Increment ||
        operation == TokenType::Decrement) {
        const std::string update =
            operation == TokenType::Increment ? "increment" : "decrement";
        const SemanticType place = PlaceType(expression.Operand());
        if (place.IsError()) {
            Report(diagnostics::DiagnosticCode::SemanticInvalidAssignmentTarget,
                   expression.Operand().Range(),
                   update + " requires a storage location");
            return SemanticType::Error();
        }
        if (place.IsConst()) {
            Report(diagnostics::DiagnosticCode::SymbolNotWritable,
                   expression.Operand().Range(),
                   update + " target is not writable");
            return SemanticType::Error();
        }
        if (!place.IsInteger()) {
            Report(diagnostics::DiagnosticCode::IncompatibleType,
                   expression.Range(),
                   update + " requires an integer storage location");
            return SemanticType::Error();
        }
        return place.WithoutConst();
    }
    if (operation == TokenType::Ampersand) {
        SemanticType target = PlaceType(expression.Operand());
        if (target.IsError() ||
            !(target.IsNumeric() || target.IsErrorValue() ||
              target.Kind() == SemanticTypeKind::Structure ||
              target.Kind() == SemanticTypeKind::Enum ||
              target.Kind() == SemanticTypeKind::Bool ||
              target.Kind() == SemanticTypeKind::Char ||
              (_allowStringReference &&
               target.Kind() == SemanticTypeKind::String) ||
              target.Kind() == SemanticTypeKind::Pointer ||
              target.Kind() == SemanticTypeKind::Array ||
              (target.Kind() == SemanticTypeKind::Tuple &&
               !target.OwnsStorage())) ||
            !IsSafePlace(expression.Operand())) {
            Report(diagnostics::DiagnosticCode::InvalidReference,
                   expression.Range(),
                   "safe borrowing requires scalar, array, or raw-pointer "
                   "storage owned by a variable or reference");
            return SemanticType::Error();
        }
        if (expected.Kind() == SemanticTypeKind::Reference &&
            expected.Target()->IsConst())
            target = target.WithConst();
        return SemanticType(SemanticTypeKind::Reference, target);
    }
    if (operation == TokenType::RawAddress) {
        RequireUnsafe(expression.Range());
        const SemanticType target = PlaceType(expression.Operand());
        if (target.IsError() || target.Kind() == SemanticTypeKind::Function ||
            target.Kind() == SemanticTypeKind::Void) {
            Report(diagnostics::DiagnosticCode::InvalidPointerAccess,
                   expression.Operand().Range(),
                   "raw address requires a variable or dereferenced storage "
                   "location");
            return SemanticType::Error();
        }
        if (target.IsBorrowed() || IsReferencePlace(expression.Operand())) {
            Report(
                diagnostics::DiagnosticCode::ReferenceEscape,
                expression.Range(),
                "raw address conversion of reference storage is not supported");
            return SemanticType::Error();
        }
        return SemanticType(SemanticTypeKind::Pointer, target);
    }
    if (operation == TokenType::Mul &&
        (operand.Kind() == SemanticTypeKind::Pointer ||
         operand.Kind() == SemanticTypeKind::Reference)) {
        if (operand.Kind() == SemanticTypeKind::Pointer)
            RequireUnsafe(expression.Range());
        if (operand.Target()->Kind() == SemanticTypeKind::Void) {
            Report(diagnostics::DiagnosticCode::InvalidPointerAccess,
                   expression.Range(), "cannot dereference a void pointer");
            return SemanticType::Error();
        }
        return operand.Target()->WithoutConst();
    }
    if ((operation == TokenType::Add || operation == TokenType::Sub) &&
        operand.IsNumeric()) {
        return operand;
    }
    if (operation == TokenType::Bang &&
        operand.Kind() == SemanticTypeKind::Bool) {
        return operand;
    }
    if (operation == TokenType::BitNot && operand.IsInteger()) return operand;
    Report(diagnostics::DiagnosticCode::InvalidOperand, expression.Range(),
           "operator is not defined for type '" + std::string(operand.Name()) +
               "'");
    return SemanticType::Error();
}

SemanticType SemanticAnalyzer::AnalyzeBinary(
    const core::BinaryExpression& expression, SemanticType expected) {
    const TokenType operation = expression.OperatorType();
    const bool shift =
        operation == TokenType::ShiftLeft || operation == TokenType::ShiftRight;
    const bool valueEquality =
        operation == TokenType::Equal || operation == TokenType::NotEqual;
    const bool strictEquality = operation == TokenType::StrictEqual ||
                                operation == TokenType::StrictNotEqual;
    SemanticType left = AnalyzeExpression(
        expression.Left(),
        expected.IsInteger() ? expected : SemanticType::Error());
    SemanticType right = AnalyzeExpression(
        expression.Right(),
        shift ? SemanticType(SemanticTypeKind::USize)
              : (left.IsInteger() ? left : SemanticType::Error()));
    if (!shift && right.IsInteger() && !expected.IsInteger() &&
        expression.Left().Kind() == core::AstNodeKind::LiteralExpression)
        left = AnalyzeExpression(expression.Left(), right);
    if (valueEquality && left.Kind() == SemanticTypeKind::None &&
        (right.ContainsStructure() || right.Kind() == SemanticTypeKind::Enum))
        left = AnalyzeExpression(expression.Left(), right);
    if (valueEquality && right.Kind() == SemanticTypeKind::None &&
        (left.ContainsStructure() || left.Kind() == SemanticTypeKind::Enum))
        right = AnalyzeExpression(expression.Right(), left);
    const bool leftStructure = left.Kind() == SemanticTypeKind::Structure;
    const bool rightStructure = right.Kind() == SemanticTypeKind::Structure;
    const bool leftNone = left.Kind() == SemanticTypeKind::None;
    const bool rightNone = right.Kind() == SemanticTypeKind::None;
    const bool leftEnum = left.Kind() == SemanticTypeKind::Enum;
    const bool rightEnum = right.Kind() == SemanticTypeKind::Enum;
    const bool leftOwner = left.Kind() == SemanticTypeKind::OwnedBuffer;
    const bool rightOwner = right.Kind() == SemanticTypeKind::OwnedBuffer;
    const bool equality =
        operation == TokenType::Equal || operation == TokenType::NotEqual;
    if (equality &&
        ((leftOwner && (right.Kind() == SemanticTypeKind::Null || rightNone)) ||
         (rightOwner && (left.Kind() == SemanticTypeKind::Null || leftNone))))
        return SemanticType(SemanticTypeKind::Bool);
    if (strictEquality && ((leftEnum && rightNone) || (leftNone && rightEnum)))
        return SemanticType(SemanticTypeKind::Bool);
    if (((valueEquality || strictEquality) && leftEnum && rightEnum &&
         left == right) ||
        (valueEquality && ((leftEnum && rightNone && left.IsZeroable()) ||
                           (leftNone && rightEnum && right.IsZeroable()))))
        return SemanticType(SemanticTypeKind::Bool);
    if (strictEquality &&
        ((leftStructure && rightNone) || (leftNone && rightStructure)))
        return SemanticType(SemanticTypeKind::Bool);
    if (((valueEquality || strictEquality) && leftStructure && rightStructure &&
         left == right) ||
        (valueEquality &&
         ((leftStructure && rightNone && left.IsZeroable()) ||
          (leftNone && rightStructure && right.IsZeroable())))) {
        const SemanticType structure = leftStructure ? left : right;
        if (IsEqualityComparable(structure))
            return SemanticType(SemanticTypeKind::Bool);
        Report(diagnostics::DiagnosticCode::InvalidOperand, expression.Range(),
               "structure '" + structure.Name() +
                   "' contains a field that cannot be compared");
        return SemanticType::Error();
    }
    if (left.ContainsStructure() || right.ContainsStructure() ||
        left.IsBorrowed() || right.IsBorrowed() || left.IsSequence() ||
        right.IsSequence() || left.Kind() == SemanticTypeKind::Tuple ||
        right.Kind() == SemanticTypeKind::Tuple) {
        Report(diagnostics::DiagnosticCode::InvalidOperand, expression.Range(),
               "arrays, tuples, and reference handles do not support binary "
               "operators");
        return SemanticType::Error();
    }
    if (left.IsError() || right.IsError()) return SemanticType::Error();
    if (left.IsErrorValue() || right.IsErrorValue()) {
        const bool strict = operation == TokenType::StrictEqual ||
                            operation == TokenType::StrictNotEqual;
        const bool equality = strict || operation == TokenType::Equal ||
                              operation == TokenType::NotEqual;
        const bool compatible =
            left == right || left.Kind() == SemanticTypeKind::None ||
            right.Kind() == SemanticTypeKind::None ||
            (left.IsErrorValue() && right.IsErrorValue() &&
             (strict || left.Kind() == SemanticTypeKind::ErrorValue ||
              right.Kind() == SemanticTypeKind::ErrorValue));
        if (equality && compatible) return SemanticType(SemanticTypeKind::Bool);
        Report(diagnostics::DiagnosticCode::InvalidOperand, expression.Range(),
               "operator is not defined for error operands '" + left.Name() +
                   "' and '" + right.Name() + "'");
        return SemanticType::Error();
    }
    const bool hasPointer = left.Kind() == SemanticTypeKind::Pointer ||
                            right.Kind() == SemanticTypeKind::Pointer;
    if (hasPointer || left.Kind() == SemanticTypeKind::Null ||
        right.Kind() == SemanticTypeKind::Null) {
        const bool equality = operation == TokenType::Equal ||
                              operation == TokenType::NotEqual ||
                              operation == TokenType::StrictEqual ||
                              operation == TokenType::StrictNotEqual;
        const bool leftZero = left.Kind() == SemanticTypeKind::Null ||
                              left.Kind() == SemanticTypeKind::None;
        const bool rightZero = right.Kind() == SemanticTypeKind::Null ||
                               right.Kind() == SemanticTypeKind::None;
        const bool strict = operation == TokenType::StrictEqual ||
                            operation == TokenType::StrictNotEqual;
        if (equality &&
            (strict ? left == right
                    : ((leftZero && rightZero) ||
                       (hasPointer &&
                        (leftZero || rightZero || IsAssignable(left, right) ||
                         IsAssignable(right, left)))))) {
            return SemanticType(SemanticTypeKind::Bool);
        }
        Report(diagnostics::DiagnosticCode::InvalidOperand, expression.Range(),
               "operator is not defined for pointer operands of these types");
        return SemanticType::Error();
    }
    const bool same = left == right;
    const bool leftString = left.Kind() == SemanticTypeKind::String ||
                            left.Kind() == SemanticTypeKind::StringView;
    const bool rightString = right.Kind() == SemanticTypeKind::String ||
                             right.Kind() == SemanticTypeKind::StringView;
    if (operation == TokenType::Add && leftString && rightString) {
        if (_currentFunction) _forbiddenZeroEffects.insert(_currentFunction);
        return SemanticType(SemanticTypeKind::String);
    }
    if (operation == TokenType::Add || operation == TokenType::Sub ||
        operation == TokenType::Mul || operation == TokenType::Div ||
        operation == TokenType::Mod) {
        if (same && left.IsNumeric() &&
            !(operation == TokenType::Mod && left.IsFloating())) {
            return left;
        }
    } else if (operation == TokenType::And || operation == TokenType::Or ||
               operation == TokenType::Xor) {
        if (same &&
            (left.Kind() == SemanticTypeKind::Bool || left.IsInteger())) {
            return left;
        }
    } else if (operation == TokenType::Ampersand ||
               operation == TokenType::BitOr) {
        if (same && left.IsInteger()) return left;
    } else if (shift) {
        if (left.IsInteger() && right.Kind() == SemanticTypeKind::USize)
            return left;
    } else if (operation == TokenType::Less ||
               operation == TokenType::LessEqual ||
               operation == TokenType::Greater ||
               operation == TokenType::GreaterEqual) {
        if (same &&
            (left.IsNumeric() || left.Kind() == SemanticTypeKind::Char)) {
            return SemanticType(SemanticTypeKind::Bool);
        }
    } else if (operation == TokenType::Equal ||
               operation == TokenType::NotEqual) {
        if ((leftString && rightString) || same ||
            left.Kind() == SemanticTypeKind::None ||
            right.Kind() == SemanticTypeKind::None) {
            return SemanticType(SemanticTypeKind::Bool);
        }
    } else if (operation == TokenType::StrictEqual ||
               operation == TokenType::StrictNotEqual) {
        return SemanticType(SemanticTypeKind::Bool);
    }
    Report(diagnostics::DiagnosticCode::InvalidOperand, expression.Range(),
           "operator is not defined for types '" + std::string(left.Name()) +
               "' and '" + std::string(right.Name()) + "'");
    return SemanticType::Error();
}

SemanticType SemanticAnalyzer::AnalyzeAssignment(
    const core::AssignmentExpression& expression) {
    const SemanticType targetValue = AnalyzeExpression(expression.Target());
    const SemanticType target = PlaceType(expression.Target());
    const TokenType operation = expression.OperatorType();
    if (operation != TokenType::Assign) {
        if (targetValue.IsError()) return SemanticType::Error();
        if (target.IsError()) {
            Report(diagnostics::DiagnosticCode::SemanticInvalidAssignmentTarget,
                   expression.Target().Range(),
                   "expression is not a storage location");
            return SemanticType::Error();
        }
        const core::Expression* place = &expression.Target();
        while (place->Kind() == core::AstNodeKind::ParenthesizedExpression) {
            place = &static_cast<const core::ParenthesizedExpression&>(*place)
                         .Inner();
        }
        const Symbol* symbol =
            place->Kind() == core::AstNodeKind::NameExpression
                ? _model.SymbolFor(
                      static_cast<const core::NameExpression&>(*place))
                : nullptr;
        if (target.IsConst() || (symbol != nullptr && !symbol->Writable())) {
            Report(diagnostics::DiagnosticCode::SymbolNotWritable,
                   expression.Target().Range(),
                   symbol != nullptr
                       ? "'" + symbol->Name() + "' is not writable"
                       : "assignment target is not writable");
            return SemanticType::Error();
        }
        const SemanticType value =
            AnalyzeExpression(expression.Value(), target.WithoutConst());
        if (value.IsError()) return SemanticType::Error();
        const bool arithmetic = operation == TokenType::AddAssign ||
                                operation == TokenType::SubAssign ||
                                operation == TokenType::MulAssign ||
                                operation == TokenType::DivAssign;
        const bool xorAssignment = operation == TokenType::XorAssign;
        const bool supported =
            target.WithoutConst() == value.WithoutConst() &&
            ((arithmetic && target.IsNumeric()) ||
             (xorAssignment &&
              (target.IsInteger() || target.Kind() == SemanticTypeKind::Bool)));
        if (!supported) {
            Report(diagnostics::DiagnosticCode::InvalidOperand,
                   expression.Range(),
                   "compound assignment operator is not defined for types '" +
                       target.Name() + "' and '" + value.Name() + "'");
            return SemanticType::Error();
        }
        return target.WithoutConst();
    }

    if (target.OwnsStorage()) RecordCleanupEffects(target);
    if (IsNoneLiteral(expression.Value()))
        MarkDirectConstruction(_directConstructions, expression.Value(), true);
    const SemanticType value = AnalyzeExpression(expression.Value(), target);
    if (IsNoneLiteral(expression.Value()))
        MarkDirectConstruction(_directConstructions, expression.Value(), false);
    CheckTransfer(expression.Value(), target);
    if (target.IsImmovable())
        Report(diagnostics::DiagnosticCode::NonCopyableValue,
               expression.Range(),
               "an immovable value cannot be replaced by assignment");
    if (_currentFunction && target.Kind() == SemanticTypeKind::String &&
        value.Kind() != SemanticTypeKind::String &&
        value.Kind() != SemanticTypeKind::None)
        _forbiddenZeroEffects.insert(_currentFunction);
    if (targetValue.IsError()) return SemanticType::Error();
    if (target.IsError()) {
        Report(diagnostics::DiagnosticCode::SemanticInvalidAssignmentTarget,
               expression.Target().Range(),
               "expression is not a storage location");
        return SemanticType::Error();
    }
    const core::Expression* place = &expression.Target();
    while (place->Kind() == core::AstNodeKind::ParenthesizedExpression) {
        place =
            &static_cast<const core::ParenthesizedExpression&>(*place).Inner();
    }
    const Symbol* symbol =
        place->Kind() == core::AstNodeKind::NameExpression
            ? _model.SymbolFor(static_cast<const core::NameExpression&>(*place))
            : nullptr;
    const core::Expression* moved = &expression.Value();
    while (moved->Kind() == core::AstNodeKind::ParenthesizedExpression)
        moved =
            &static_cast<const core::ParenthesizedExpression&>(*moved).Inner();
    if (symbol != nullptr &&
        moved->Kind() == core::AstNodeKind::NameExpression &&
        _model.TransferOf(*moved) == TransferKind::Move &&
        _model.SymbolFor(static_cast<const core::NameExpression&>(*moved)) ==
            symbol) {
        Report(diagnostics::DiagnosticCode::NonCopyableValue,
               expression.Value().Range(),
               "cannot move '" + symbol->Name() + "' into itself");
        return SemanticType::Error();
    }
    if (target.IsConst() || (symbol != nullptr && !symbol->Writable())) {
        Report(diagnostics::DiagnosticCode::SymbolNotWritable,
               expression.Target().Range(),
               symbol != nullptr ? "'" + symbol->Name() + "' is not writable"
                                 : "assignment target is not writable");
        return SemanticType::Error();
    }
    if (!IsAssignable(target, value)) {
        Report(diagnostics::DiagnosticCode::IncompatibleType,
               expression.Value().Range(), TypeMismatch(target, value));
        return SemanticType::Error();
    }
    if (symbol != nullptr && target.Kind() == SemanticTypeKind::StringView) {
        if (BorrowsLocalString(expression.Value())) {
            _localStringBorrows.insert(symbol);
        } else {
            _localStringBorrows.erase(symbol);
        }
    }
    return target.WithoutConst();
}

bool SemanticAnalyzer::BorrowsLocalString(
    const core::Expression& expression) const {
    switch (expression.Kind()) {
        case core::AstNodeKind::NameExpression: {
            const Symbol* symbol = _model.SymbolFor(
                static_cast<const core::NameExpression&>(expression));
            if (symbol == nullptr) return false;
            if (_localStringBorrows.contains(symbol)) return true;
            return symbol->Type().Kind() == SemanticTypeKind::String;
        }
        case core::AstNodeKind::ParenthesizedExpression:
            return BorrowsLocalString(
                static_cast<const core::ParenthesizedExpression&>(expression)
                    .Inner());
        case core::AstNodeKind::CastExpression: {
            const core::CastExpression& cast =
                static_cast<const core::CastExpression&>(expression);
            return _model.TypeOf(cast).Kind() == SemanticTypeKind::StringView &&
                   (_model.TypeOf(cast.Value()).Kind() ==
                        SemanticTypeKind::String ||
                    BorrowsLocalString(cast.Value()));
        }
        case core::AstNodeKind::MemberCallExpression: {
            const core::MemberCallExpression& call =
                static_cast<const core::MemberCallExpression&>(expression);
            if (call.Member() != "view" && call.Member() != "substr") {
                return false;
            }
            return _model.TypeOf(call.Receiver()).Kind() ==
                       SemanticTypeKind::String ||
                   BorrowsLocalString(call.Receiver());
        }
        case core::AstNodeKind::MatchExpression: {
            const auto& match =
                static_cast<const core::MatchExpression&>(expression);
            for (const auto& arm : match.Arms())
                if (BorrowsLocalString(arm->Value())) return true;
            return false;
        }
        default:
            return false;
    }
}

SemanticType SemanticAnalyzer::AnalyzeAllocationCall(
    const core::CallExpression& expression, BuiltInFunction intrinsic) {
    if (_inZero)
        Report(diagnostics::DiagnosticCode::InvalidLifecycle,
               expression.Range(), "zero cannot allocate or free storage");
    if (_currentFunction) _forbiddenZeroEffects.insert(_currentFunction);
    const bool allocate = intrinsic == BuiltInFunction::New ||
                          intrinsic == BuiltInFunction::StorageNew;
    const bool uninitialized = intrinsic == BuiltInFunction::StorageNew;
    const bool crashesOnFailure =
        intrinsic == BuiltInFunction::New && expression.IsCrashable();
    if (expression.IsCrashable() && intrinsic != BuiltInFunction::New)
        Report(diagnostics::DiagnosticCode::InvalidCrashable,
               expression.Range(),
               "only new<T> supports a crash-on-allocation-failure form");
    if (crashesOnFailure) {
        if (!_currentFunction || !_currentFunction->IsCrashable())
            Report(diagnostics::DiagnosticCode::InvalidCrashable,
                   expression.Range(),
                   "new<T>!() may only appear in a crashable function");
        else
            _directCrashFunctions.insert(_currentFunction);
    }
    SemanticType element = SemanticType::Error();
    const bool dynamic =
        allocate && expression.TypeArgument() &&
        expression.TypeArgument()->TypeForm() == core::TypeKind::Slice;
    const bool safeOwner =
        !uninitialized && dynamic &&
        expression.TypeArgument()->Contract() == core::OwnershipContract::Own;
    if (!safeOwner) RequireUnsafe(expression.Range());
    if (allocate) {
        if (!expression.TypeArgument())
            Report(diagnostics::DiagnosticCode::InvalidAllocation,
                   expression.Range(), "allocation requires new<T>(value)");
        else {
            if (uninitialized && !dynamic)
                Report(diagnostics::DiagnosticCode::InvalidAllocation,
                       expression.TypeArgument()->Range(),
                       "__storage_new requires an uninitialized []T buffer");
            element = ResolveType(dynamic ? *expression.TypeArgument()->Target()
                                          : *expression.TypeArgument());
            if (dynamic && expression.TypeArgument()->IsConst())
                element = element.WithConst();
            if (dynamic && !uninitialized &&
                (!StorageSize(element) || !element.IsZeroable() ||
                 element.IsImmovable()))
                Report(diagnostics::DiagnosticCode::InvalidAllocation,
                       expression.TypeArgument()->Range(),
                       "runtime arrays require sized, safely movable, "
                       "semantically zeroable elements");
            if (dynamic && !uninitialized && element.IsZeroable() &&
                !element.IsTriviallyZeroable())
                WarnForNontrivialDefault(element,
                                         expression.TypeArgument()->Range());
            if (uninitialized && element.IsImmovable())
                Report(diagnostics::DiagnosticCode::InvalidAllocation,
                       expression.TypeArgument()->Range(),
                       "uninitialized collection storage requires movable "
                       "elements");
            if (!element.IsError() && !StorageSize(element) &&
                (!dynamic || uninitialized))
                Report(
                    diagnostics::DiagnosticCode::InvalidAllocation,
                    expression.TypeArgument()->Range(),
                    "allocation requires a sized owned value type; references, "
                    "slices, stringv, and void cannot be allocated");
        }
    }
    if (expression.Arguments().size() != 1)
        Report(
            diagnostics::DiagnosticCode::WrongArgumentCount, expression.Range(),
            allocate ? (dynamic ? (uninitialized
                                       ? "__storage_new<[]T> requires exactly "
                                         "one usize count"
                                       : "new<[]T> requires exactly one usize "
                                         "count")
                                : "new<T> requires exactly one initializer")
                     : "free requires exactly one raw pointer");
    const SemanticType input =
        dynamic ? SemanticType(SemanticTypeKind::USize) : element;
    SemanticType argument = SemanticType::Error();
    for (const auto& value : expression.Arguments()) {
        if (allocate && !dynamic)
            MarkDirectConstruction(_directConstructions, *value, true);
        argument =
            AnalyzeExpression(*value, allocate ? input : SemanticType::Error());
        if (allocate && !dynamic)
            MarkDirectConstruction(_directConstructions, *value, false);
        if (allocate) {
            CheckTransfer(*value, input);
            if (!IsAssignable(input, argument))
                Report(diagnostics::DiagnosticCode::IncompatibleArgument,
                       value->Range(), TypeMismatch(input, argument));
        } else if (_currentFunction != nullptr && _currentFunction->IsOnce()) {
            const core::Expression* source = value.get();
            while (source->Kind() == core::AstNodeKind::ParenthesizedExpression)
                source =
                    &static_cast<const core::ParenthesizedExpression&>(*source)
                         .Inner();
            if (source->Kind() == core::AstNodeKind::NameExpression) {
                const Symbol* symbol = _model.SymbolFor(
                    static_cast<const core::NameExpression&>(*source));
                const auto* capture =
                    symbol != nullptr ? _model.CaptureFor(*symbol) : nullptr;
                if (capture != nullptr &&
                    capture->Mode() ==
                        core::AnonymousFunctionExpression::CaptureMode::Own)
                    CheckTransfer(*value, argument, true);
            }
        }
    }
    SemanticType parameter = input;
    SemanticType result(SemanticTypeKind::Void);
    if (allocate) {
        const SemanticType owner(
            dynamic ? (safeOwner ? SemanticTypeKind::OwnedSlice
                                 : SemanticTypeKind::OwnedBuffer)
                    : SemanticTypeKind::Pointer,
            element);
        result = crashesOnFailure
                     ? owner
                     : SemanticType(std::vector<SemanticType>{
                           owner, SemanticType(SemanticTypeKind::ErrorValue)});
    } else {
        if (argument.Kind() == SemanticTypeKind::Null ||
            argument.Kind() == SemanticTypeKind::None)
            parameter = SemanticType(SemanticTypeKind::Pointer,
                                     SemanticType(SemanticTypeKind::U8));
        else if ((argument.Kind() == SemanticTypeKind::Pointer ||
                  argument.Kind() == SemanticTypeKind::OwnedBuffer) &&
                 StorageSize(*argument.Target()))
            parameter = argument;
        else if (!argument.IsError())
            Report(diagnostics::DiagnosticCode::InvalidAllocation,
                   expression.Range(),
                   "free requires a raw pointer to a sized owned value type, "
                   "or null");
    }
    // Each call has a concrete typed signature, also used for deferred free.
    const auto& callee =
        static_cast<const core::NameExpression&>(expression.Callee());
    auto signature = std::make_shared<CallableSignature>();
    signature->result = result;
    signature->parameters = {parameter};
    signature->isUnsafe = true;
    signature->isCrashable = crashesOnFailure;
    auto specialized = std::make_unique<FunctionSymbol>(
        callee.Name(), std::move(signature), callee.Range(), intrinsic);
    _model._bindings[&callee] = specialized.get();
    _model._types.insert_or_assign(&callee, specialized->Type());
    _model._symbols.push_back(std::move(specialized));
    return result;
}

SemanticType SemanticAnalyzer::AnalyzeStorageCall(
    const core::CallExpression& expression, BuiltInFunction intrinsic) {
    if (_inZero)
        Report(diagnostics::DiagnosticCode::InvalidLifecycle,
               expression.Range(),
               "zero cannot manipulate uninitialized collection storage");
    if (_currentFunction) _forbiddenZeroEffects.insert(_currentFunction);
    RequireUnsafe(expression.Range());

    const std::size_t required =
        intrinsic == BuiltInFunction::StorageRelocate     ? 4
        : intrinsic == BuiltInFunction::StorageInitialize ? 3
                                                          : 2;
    if (expression.Arguments().size() != required)
        Report(diagnostics::DiagnosticCode::WrongArgumentCount,
               expression.Range(),
               "collection storage intrinsic requires exactly " +
                   std::to_string(required) + " arguments");

    std::vector<SemanticType> parameters;
    parameters.reserve(expression.Arguments().size());
    SemanticType element = SemanticType::Error();
    const auto analyzeStorage = [&](std::size_t index) {
        if (index >= expression.Arguments().size())
            return SemanticType::Error();
        const SemanticType storage =
            AnalyzeExpression(*expression.Arguments()[index]);
        parameters.push_back(storage);
        if (storage.Kind() != SemanticTypeKind::Pointer &&
            storage.Kind() != SemanticTypeKind::OwnedBuffer) {
            Report(diagnostics::DiagnosticCode::InvalidAllocation,
                   expression.Arguments()[index]->Range(),
                   "collection storage requires an allocated buffer");
            return SemanticType::Error();
        }
        const SemanticType target = *storage.Target();
        if (target.IsConst())
            Report(diagnostics::DiagnosticCode::InvalidAllocation,
                   expression.Arguments()[index]->Range(),
                   "collection storage requires mutable elements");
        return target.WithoutConst();
    };
    const auto analyzeIndex = [&](std::size_t index) {
        if (index >= expression.Arguments().size()) return;
        const SemanticType expected(SemanticTypeKind::USize);
        const SemanticType actual =
            AnalyzeExpression(*expression.Arguments()[index], expected);
        parameters.push_back(expected);
        if (!IsAssignable(expected, actual))
            Report(diagnostics::DiagnosticCode::IncompatibleArgument,
                   expression.Arguments()[index]->Range(),
                   TypeMismatch(expected, actual));
    };

    element = analyzeStorage(0);
    analyzeIndex(1);
    if (intrinsic == BuiltInFunction::StorageExtract && !element.IsError() &&
        element.IsImmovable())
        Report(diagnostics::DiagnosticCode::InvalidAllocation,
               expression.Arguments()[0]->Range(),
               "collection storage cannot extract an immovable element");
    if (intrinsic == BuiltInFunction::StorageInitialize) {
        if (expression.Arguments().size() > 2) {
            const SemanticType value =
                AnalyzeExpression(*expression.Arguments()[2], element);
            parameters.push_back(element);
            if (!element.IsError() && !IsAssignable(element, value))
                Report(diagnostics::DiagnosticCode::IncompatibleArgument,
                       expression.Arguments()[2]->Range(),
                       TypeMismatch(element, value));
            else if (!element.IsError())
                CheckTransfer(*expression.Arguments()[2], element);
        }
    } else if (intrinsic == BuiltInFunction::StorageRelocate) {
        const SemanticType source = analyzeStorage(2);
        analyzeIndex(3);
        if (!element.IsError() && !source.IsError() && element != source)
            Report(diagnostics::DiagnosticCode::IncompatibleArgument,
                   expression.Arguments()[2]->Range(),
                   "relocation requires matching source and destination "
                   "element types");
    }

    const auto& callee =
        static_cast<const core::NameExpression&>(expression.Callee());
    const SemanticType result = intrinsic == BuiltInFunction::StorageExtract
                                    ? element
                                    : SemanticType(SemanticTypeKind::Void);
    auto specialized = std::make_unique<FunctionSymbol>(
        callee.Name(), result, std::move(parameters), callee.Range(), intrinsic,
        true);
    _model._bindings[&callee] = specialized.get();
    _model._types.insert_or_assign(&callee, specialized->Type());
    _model._symbols.push_back(std::move(specialized));
    return result;
}

SemanticType SemanticAnalyzer::AnalyzeHashCall(
    const core::CallExpression& expression) {
    SemanticType value = SemanticType::Error();
    if (!expression.TypeArgument()) {
        Report(diagnostics::DiagnosticCode::InvalidOperand, expression.Range(),
               "hashing requires __hash<T>(value)");
    } else {
        value = ResolveType(*expression.TypeArgument());
        if (!value.IsError() && !IsEqualityComparable(value))
            Report(diagnostics::DiagnosticCode::InvalidOperand,
                   expression.TypeArgument()->Range(),
                   "hashing requires an equality-comparable value type");
    }

    if (expression.Arguments().size() != 1)
        Report(diagnostics::DiagnosticCode::WrongArgumentCount,
               expression.Range(),
               "__hash<T> requires exactly one borrowed value");

    SemanticType parameter(SemanticTypeKind::Reference,
                           value.IsError() ? value : value.WithConst());
    for (std::size_t i = 0; i < expression.Arguments().size(); ++i) {
        const auto& argument = expression.Arguments()[i];
        const SemanticType actual = AnalyzeExpression(*argument);
        if (i != 0) continue;
        const bool direct = !value.IsError() && IsAssignable(value, actual);
        const bool referenced =
            !value.IsError() && actual.Kind() == SemanticTypeKind::Reference &&
            actual.Target()->WithoutConst() == value.WithoutConst();
        if (!direct && !referenced && !actual.IsError())
            Report(diagnostics::DiagnosticCode::IncompatibleArgument,
                   argument->Range(),
                   "hash argument must be the selected value or a reference "
                   "to it");
        parameter = actual;
    }

    const auto& callee =
        static_cast<const core::NameExpression&>(expression.Callee());
    const SemanticType result(SemanticTypeKind::USize);
    auto specialized = std::make_unique<FunctionSymbol>(
        callee.Name(), result, std::vector<SemanticType>{parameter},
        callee.Range(), BuiltInFunction::Hash);
    _model._bindings[&callee] = specialized.get();
    _model._types.insert_or_assign(&callee, specialized->Type());
    _model._symbols.push_back(std::move(specialized));
    return result;
}

SemanticType SemanticAnalyzer::AnalyzeJsonSliceAppendCall(
    const core::CallExpression& expression) {
    SemanticType element = SemanticType::Error();
    if (!expression.TypeArgument()) {
        Report(diagnostics::DiagnosticCode::InvalidAllocation,
               expression.Range(),
               "JSON slice append requires an element type argument");
    } else {
        element = ResolveType(*expression.TypeArgument());
        if (!element.IsError() &&
            (!StorageSize(element) || element.IsImmovable()))
            Report(diagnostics::DiagnosticCode::InvalidAllocation,
                   expression.TypeArgument()->Range(),
                   "JSON slice append requires a sized movable element type");
    }

    if (expression.Arguments().size() != 2)
        Report(diagnostics::DiagnosticCode::WrongArgumentCount,
               expression.Range(),
               "JSON slice append requires an owning slice and one element");

    const SemanticType slice(SemanticTypeKind::OwnedSlice, element);
    const std::array expected{slice, element};
    for (std::size_t index = 0; index < expression.Arguments().size();
         ++index) {
        const SemanticType target =
            index < expected.size() ? expected[index] : SemanticType::Error();
        const SemanticType actual =
            AnalyzeExpression(*expression.Arguments()[index], target);
        if (!target.IsError() && !IsAssignable(target, actual))
            Report(diagnostics::DiagnosticCode::IncompatibleArgument,
                   expression.Arguments()[index]->Range(),
                   TypeMismatch(target, actual));
        if (!target.IsError())
            CheckTransfer(*expression.Arguments()[index], target, true);
    }

    const SemanticType result(std::vector<SemanticType>{
        slice, SemanticType(SemanticTypeKind::ErrorValue)});
    const auto& callee =
        static_cast<const core::NameExpression&>(expression.Callee());
    auto signature = std::make_shared<CallableSignature>();
    signature->result = result;
    signature->parameters = {slice, element};
    signature->parameterContracts = {core::OwnershipContract::Own,
                                     core::OwnershipContract::Own};
    auto specialized = std::make_unique<FunctionSymbol>(
        callee.Name(), std::move(signature), callee.Range(),
        BuiltInFunction::JsonSliceAppend);
    _model._bindings[&callee] = specialized.get();
    _model._types.insert_or_assign(&callee, specialized->Type());
    _model._symbols.push_back(std::move(specialized));
    return result;
}

void SemanticAnalyzer::ValidateCrashableCall(bool markedCrashable,
                                             const CallableSignature& signature,
                                             const FunctionSymbol* function,
                                             source::SourceRange range) {
    if (function && function->BuiltIn() == BuiltInFunction::Crash) {
        if (markedCrashable)
            Report(diagnostics::DiagnosticCode::InvalidCrashable, range,
                   "crash is an explicit terminal operation and is called "
                   "as crash(), without a bang");
        if (!_currentFunction || !_currentFunction->IsCrashable()) {
            Report(diagnostics::DiagnosticCode::InvalidCrashable, range,
                   "crash() may only be called from a crashable function");
        } else {
            _directCrashFunctions.insert(_currentFunction);
        }
        return;
    }

    if (signature.isCrashable != markedCrashable) {
        if (signature.isCrashable)
            Report(diagnostics::DiagnosticCode::InvalidCrashable, range,
                   "a crashable function must be called with '!'");
        else
            Report(diagnostics::DiagnosticCode::InvalidCrashable, range,
                   "a non-crashable function cannot be called with '!'");
        return;
    }
    if (!markedCrashable) return;

    if (!_currentFunction || !_currentFunction->IsCrashable()) {
        Report(diagnostics::DiagnosticCode::InvalidCrashable, range,
               "a crashable call may only appear in a crashable function");
        return;
    }

    if (!function || function->BuiltIn() == BuiltInFunction::StringFormatCrash)
        _directCrashFunctions.insert(_currentFunction);
    else
        _crashableCalls[_currentFunction].insert(function);
}

void SemanticAnalyzer::WarnForCrashlessFunctions() {
    std::unordered_set<const FunctionSymbol*> canCrash = _directCrashFunctions;
    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto& [caller, callees] : _crashableCalls) {
            if (canCrash.contains(caller)) continue;
            if (std::ranges::any_of(callees, [&](const FunctionSymbol* callee) {
                    return canCrash.contains(callee);
                })) {
                canCrash.insert(caller);
                changed = true;
            }
        }
    }

    for (const auto& symbol : _model._symbols) {
        if (symbol->Kind() != SymbolKind::Function) continue;
        const auto* function = static_cast<const FunctionSymbol*>(symbol.get());
        if (!function->IsCrashable() || function->Declaration() == nullptr ||
            !function->Declaration()->IsCrashable() ||
            function->InterfaceOwner() != nullptr ||
            canCrash.contains(function))
            continue;
        _diagnostics.Report(
            diagnostics::DiagnosticCode::InvalidCrashable,
            function->DeclarationRange(),
            "method '" + function->Declaration()->Name() +
                "' is marked as crashable, while no path leads to a "
                "crashable state",
            diagnostics::DiagnosticSeverity::Warning);
    }
}

std::vector<const FunctionSymbol*> SemanticAnalyzer::LookupFunctions(
    std::string_view name) const {
    std::vector<const FunctionSymbol*> result;
    std::set<const FunctionSymbol*> seen;
    for (const Symbol* symbol : _currentScope->LookupAll(name)) {
        if (symbol->Kind() != SymbolKind::Function) return {};
        const auto* function = static_cast<const FunctionSymbol*>(symbol);
        if (seen.insert(function).second) result.push_back(function);
    }
    return result;
}

const FunctionSymbol* SemanticAnalyzer::ResolveOverload(
    std::string_view displayName,
    const std::vector<const FunctionSymbol*>& candidates,
    const std::vector<std::unique_ptr<core::Expression>>& arguments,
    source::SourceRange range, bool isStatic, SemanticType receiver) {
    std::vector<SemanticType> actualTypes;
    actualTypes.reserve(arguments.size());
    for (const auto& argument : arguments)
        actualTypes.push_back(AnalyzeExpression(*argument));

    const auto isNumericLiteral = [](const core::Expression& expression) {
        const core::Expression* value = &expression;
        while (value->Kind() == core::AstNodeKind::ParenthesizedExpression)
            value = &static_cast<const core::ParenthesizedExpression&>(*value)
                         .Inner();
        if (value->Kind() == core::AstNodeKind::UnaryExpression) {
            const auto& unary =
                static_cast<const core::UnaryExpression&>(*value);
            if (unary.OperatorType() != TokenType::Add &&
                unary.OperatorType() != TokenType::Sub)
                return false;
            value = &unary.Operand();
            while (value->Kind() == core::AstNodeKind::ParenthesizedExpression)
                value =
                    &static_cast<const core::ParenthesizedExpression&>(*value)
                         .Inner();
        }
        if (value->Kind() != core::AstNodeKind::LiteralExpression) return false;
        const auto token =
            static_cast<const core::LiteralExpression&>(*value).LiteralType();
        return token == TokenType::NumberLiteral ||
               token == TokenType::FloatLiteral;
    };

    struct Match final {
        const FunctionSymbol* function;
        std::size_t score;
    };
    std::vector<Match> matches;
    for (const FunctionSymbol* candidate : candidates) {
        if (candidate->HasReceiver() == isStatic) continue;
        const std::size_t offset = candidate->HasReceiver() ? 1 : 0;
        if (candidate->HasReceiver() && receiver.IsError()) continue;

        const auto& parameters = candidate->ParameterTypes();
        const std::size_t maximum = parameters.size() - offset;
        std::size_t minimum = maximum;
        if (const auto* declaration = candidate->Declaration()) {
            for (std::size_t index = offset;
                 index < declaration->Parameters().size(); ++index) {
                if (declaration->Parameters()[index]->DefaultValue()) {
                    minimum = index - offset;
                    break;
                }
            }
        }
        if (arguments.size() < minimum || arguments.size() > maximum) continue;

        std::size_t score = 0;
        bool viable = true;
        if (candidate->HasReceiver()) {
            const SemanticType expected = parameters.front();
            const SemanticType expectedTarget =
                expected.Target() ? *expected.Target() : expected;
            const SemanticType actualTarget =
                receiver.Target() ? *receiver.Target() : receiver;
            if (actualTarget.IsConst() && !expectedTarget.IsConst())
                viable = false;
            else if (!actualTarget.IsConst() && expectedTarget.IsConst())
                ++score;
        }
        if (!viable) continue;
        for (std::size_t index = 0; index < actualTypes.size(); ++index) {
            const SemanticType expected = parameters[index + offset];
            const SemanticType actual = actualTypes[index];
            if (actual == expected ||
                actual.WithoutConst() == expected.WithoutConst())
                continue;
            if (IsAssignable(expected, actual) ||
                CanReborrow(expected, actual)) {
                ++score;
                continue;
            }
            if (isNumericLiteral(*arguments[index]) && expected.IsNumeric()) {
                score += 2;
                continue;
            }
            viable = false;
            break;
        }
        if (viable) matches.push_back({candidate, score});
    }

    if (matches.empty()) {
        Report(diagnostics::DiagnosticCode::IncompatibleArgument, range,
               "no overload of '" + std::string(displayName) +
                   "' accepts the supplied argument types");
        return nullptr;
    }
    const auto best =
        std::min_element(matches.begin(), matches.end(),
                         [](const Match& left, const Match& right) {
                             return left.score < right.score;
                         });
    if (std::count_if(matches.begin(), matches.end(), [&](const Match& match) {
            return match.score == best->score;
        }) != 1) {
        Report(diagnostics::DiagnosticCode::IncompatibleArgument, range,
               "ambiguous overload of '" + std::string(displayName) + "'");
        return nullptr;
    }
    return best->function;
}

SemanticType SemanticAnalyzer::AnalyzeCall(
    const core::CallExpression& expression) {
    if (expression.Callee().Kind() == core::AstNodeKind::NameExpression &&
        static_cast<const core::NameExpression&>(expression.Callee()).Name() ==
            "take" &&
        !_currentScope->Lookup("take")) {
        if (_inZero)
            Report(diagnostics::DiagnosticCode::InvalidLifecycle,
                   expression.Range(),
                   "zero cannot perform ownership operations");
        if (_currentFunction) _forbiddenZeroEffects.insert(_currentFunction);
        RequireUnsafe(expression.Range());
        if (expression.Arguments().size() != 1) {
            Report(diagnostics::DiagnosticCode::WrongArgumentCount,
                   expression.Range(),
                   "take requires exactly one owning self field");
            return SemanticType::Error();
        }
        const auto& argument = *expression.Arguments()[0];
        const auto type = AnalyzeExpression(argument);
        const auto place = PlaceType(argument);
        const core::Expression* unwrapped = &argument;
        while (unwrapped->Kind() == core::AstNodeKind::ParenthesizedExpression)
            unwrapped =
                &static_cast<const core::ParenthesizedExpression&>(*unwrapped)
                     .Inner();
        bool valid = false;
        if (unwrapped->Kind() == core::AstNodeKind::MemberAccessExpression) {
            const auto& member =
                static_cast<const core::MemberAccessExpression&>(*unwrapped);
            const auto* field = _model.FieldFor(member);
            const auto* receiver = &member.Receiver();
            while (receiver->Kind() ==
                   core::AstNodeKind::ParenthesizedExpression)
                receiver = &static_cast<const core::ParenthesizedExpression&>(
                                *receiver)
                                .Inner();
            valid =
                field &&
                field->syntax->Type().Contract() ==
                    core::OwnershipContract::Own &&
                receiver->Kind() == core::AstNodeKind::NameExpression &&
                static_cast<const core::NameExpression&>(*receiver).Name() ==
                    "self" &&
                CurrentOwner() && _currentFunction->HasReceiver() &&
                _model.SymbolFor(
                    static_cast<const core::NameExpression&>(*receiver)) &&
                _model.SymbolFor(
                          static_cast<const core::NameExpression&>(*receiver))
                        ->Kind() == SymbolKind::Parameter &&
                member.Access() == TokenType::Arrow && !place.IsConst();
        }
        if (!valid)
            Report(
                diagnostics::DiagnosticCode::UnverifiedOwnership,
                argument.Range(),
                "take requires a mutable owning field accessed through self");
        const auto& callee =
            static_cast<const core::NameExpression&>(expression.Callee());
        auto specialized = std::make_unique<FunctionSymbol>(
            callee.Name(), type, std::vector<SemanticType>{type},
            callee.Range(), BuiltInFunction::Take, true);
        _model._bindings[&callee] = specialized.get();
        _model._types.insert_or_assign(&callee, specialized->Type());
        _model._symbols.push_back(std::move(specialized));
        return type;
    }
    const FunctionSymbol* function = nullptr;
    SemanticType calleeType = SemanticType::Error();
    if (expression.Callee().Kind() == core::AstNodeKind::NameExpression) {
        const auto& callee =
            static_cast<const core::NameExpression&>(expression.Callee());
        const std::string lookupName =
            EffectName(callee.Name(), expression.IsCrashable());
        const auto overloads = LookupFunctions(lookupName);
        if (overloads.size() > 1) {
            function =
                ResolveOverload(callee.Name(), overloads,
                                expression.Arguments(), expression.Range());
            if (!function) return SemanticType::Error();
            _model._bindings.insert_or_assign(&callee, function);
            _model._types.insert_or_assign(&callee, function->Type());
            calleeType = function->Type();
        }
    }
    if (!function) {
        const bool previousCallee = _analyzingCallCallee;
        const bool previousCrashableCallee = _callCalleeCrashable;
        _analyzingCallCallee = true;
        _callCalleeCrashable = expression.IsCrashable();
        calleeType = AnalyzeExpression(expression.Callee());
        _analyzingCallCallee = previousCallee;
        _callCalleeCrashable = previousCrashableCallee;
        if (expression.Callee().Kind() == core::AstNodeKind::NameExpression) {
            const Symbol* symbol = _model.SymbolFor(
                static_cast<const core::NameExpression&>(expression.Callee()));
            if (symbol != nullptr && symbol->Kind() == SymbolKind::Function)
                function = static_cast<const FunctionSymbol*>(symbol);
        }
    }
    const FunctionSymbol* declaredFunction = function;
    std::vector<const FunctionSymbol*> variadicFunctions;
    std::optional<std::size_t> variadicMinimumArguments;
    if (function && function->Declaration() &&
        function->Declaration()->HasVariadicPack()) {
        variadicMinimumArguments = function->ParameterTypes().size();
        for (std::size_t index = 0; index < expression.Arguments().size();
             ++index)
            if (expression.ArgumentIsExpanded(index))
                Report(diagnostics::DiagnosticCode::InvalidVariadic,
                       expression.Arguments()[index]->Range(),
                       "callers pass ordinary arguments; only a variadic "
                       "function body may expand its pack");

        std::unordered_set<const FunctionSymbol*> seen;
        while (function && function->Declaration() &&
               function->Declaration()->HasVariadicPack()) {
            if (!seen.insert(function).second) {
                Report(diagnostics::DiagnosticCode::InvalidVariadic,
                       expression.Callee().Range(),
                       "variadic forwarding cycle involving '" +
                           declaredFunction->Name() + "'");
                return SemanticType::Error();
            }
            variadicFunctions.push_back(function);
            *variadicMinimumArguments = std::max(
                *variadicMinimumArguments, function->ParameterTypes().size());
            const auto target = _model._variadicTargets.find(function);
            if (target == _model._variadicTargets.end())
                return SemanticType::Error();
            function = target->second;
        }
        if (function != nullptr) {
            calleeType = function->Type();
            _model._forwardedFunctions.insert_or_assign(&expression, function);
        }
    } else {
        for (std::size_t index = 0; index < expression.Arguments().size();
             ++index)
            if (expression.ArgumentIsExpanded(index))
                Report(diagnostics::DiagnosticCode::InvalidVariadic,
                       expression.Arguments()[index]->Range(),
                       "argument expansion is only valid for a declared "
                       "variadic pack inside its forwarding function");
    }
    if (function && (function->BuiltIn() == BuiltInFunction::New ||
                     function->BuiltIn() == BuiltInFunction::Free ||
                     function->BuiltIn() == BuiltInFunction::StorageNew ||
                     function->BuiltIn() == BuiltInFunction::StorageFree))
        return AnalyzeAllocationCall(expression, function->BuiltIn());
    if (function && function->BuiltIn() == BuiltInFunction::Hash)
        return AnalyzeHashCall(expression);
    if (function && function->BuiltIn() == BuiltInFunction::JsonSliceAppend)
        return AnalyzeJsonSliceAppendCall(expression);
    if (function &&
        (function->BuiltIn() == BuiltInFunction::StorageInitialize ||
         function->BuiltIn() == BuiltInFunction::StorageRelocate ||
         function->BuiltIn() == BuiltInFunction::StorageExtract ||
         function->BuiltIn() == BuiltInFunction::StorageDestroy))
        return AnalyzeStorageCall(expression, function->BuiltIn());
    if (expression.TypeArgument())
        Report(diagnostics::DiagnosticCode::InvalidAllocation,
               expression.Range(),
               "new<T> must resolve to the allocation built-in");
    const CallableSignature* signature =
        calleeType.Kind() == SemanticTypeKind::Function
            ? &calleeType.Signature()
            : nullptr;
    if (signature != nullptr && signature->isOnce)
        CheckTransfer(expression.Callee(), calleeType);
    std::vector<const core::Expression*> arguments;
    arguments.reserve(expression.Arguments().size());
    for (const auto& argument : expression.Arguments())
        arguments.push_back(argument.get());
    std::optional<std::size_t> minimumArguments;
    if (function && function->BuiltIn() == BuiltInFunction::None &&
        function->Declaration() && signature) {
        const auto& parameters = function->Declaration()->Parameters();
        std::size_t required = parameters.size();
        for (std::size_t index = 0; index < parameters.size(); ++index)
            if (parameters[index]->DefaultValue() != nullptr) {
                required = index;
                break;
            }
        minimumArguments = required;
        if (arguments.size() >= required &&
            arguments.size() <= parameters.size()) {
            for (std::size_t index = arguments.size();
                 index < parameters.size(); ++index)
                arguments.push_back(parameters[index]->DefaultValue());
            if (arguments.size() != expression.Arguments().size())
                _model._callArguments[&expression] = arguments;
        }
    }
    std::vector<SemanticType> argumentTypes;
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        const SemanticType expected =
            signature != nullptr &&
                    (!function ||
                     function->BuiltIn() == BuiltInFunction::None) &&
                    index < signature->parameters.size()
                ? signature->parameters[index]
                : SemanticType::Error();
        const bool provided = index < expression.Arguments().size();
        const bool inputDestination =
            function &&
            (function->BuiltIn() == BuiltInFunction::Stdin ||
             function->BuiltIn() == BuiltInFunction::StdinLine) &&
            index + 1 == arguments.size();
        const bool previousStringReference = _allowStringReference;
        _allowStringReference = inputDestination;
        argumentTypes.push_back(
            provided ? AnalyzeExpression(*arguments[index], expected)
                     : _model.TypeOf(*arguments[index]));
        _allowStringReference = previousStringReference;
        const bool consuming = signature != nullptr &&
                               index < signature->parameterContracts.size() &&
                               signature->parameterContracts[index] ==
                                   core::OwnershipContract::Own;
        if (provided) CheckTransfer(*arguments[index], expected, consuming);
    }
    if (declaredFunction != nullptr && variadicMinimumArguments.has_value()) {
        if (expression.Arguments().size() < *variadicMinimumArguments)
            Report(diagnostics::DiagnosticCode::WrongArgumentCount,
                   expression.Range(),
                   "variadic function '" + declaredFunction->Name() +
                       "' expects at least " +
                       std::to_string(*variadicMinimumArguments) +
                       " argument(s), found " +
                       std::to_string(expression.Arguments().size()));
        for (const FunctionSymbol* variadic : variadicFunctions) {
            const std::size_t fixedCount =
                std::min(expression.Arguments().size(),
                         variadic->ParameterTypes().size());
            for (std::size_t index = 0; index < fixedCount; ++index) {
                const SemanticType expected = variadic->ParameterTypes()[index];
                if (!IsAssignable(expected, argumentTypes[index]) &&
                    !CanReborrow(expected, argumentTypes[index]))
                    Report(diagnostics::DiagnosticCode::IncompatibleArgument,
                           expression.Arguments()[index]->Range(),
                           "argument " + std::to_string(index + 1) + ": " +
                               TypeMismatch(expected, argumentTypes[index]));
            }
        }
        if (declaredFunction->BorrowsVariadicPack()) {
            for (std::size_t index = declaredFunction->ParameterTypes().size();
                 index < argumentTypes.size(); ++index)
                if (!argumentTypes[index].IsError() &&
                    !CanCarryBorrow(argumentTypes[index]))
                    Report(diagnostics::DiagnosticCode::InvalidBorrowContract,
                           expression.Arguments()[index]->Range(),
                           "expanded borrow source must be able to carry a "
                           "borrow");
        }
    }
    if (calleeType.IsError()) return SemanticType::Error();
    if (signature == nullptr) {
        Report(diagnostics::DiagnosticCode::NotCallable,
               expression.Callee().Range(), "expression is not callable");
        return SemanticType::Error();
    }
    ValidateCrashableCall(expression.IsCrashable(), *signature, function,
                          expression.Range());
    if (signature->isUnsafe || (signature->result.IsImmovable() && !_inZero))
        RequireUnsafe(expression.Callee().Range());
    if (_currentFunction && function)
        _functionCalls[_currentFunction].insert(function);
    if (function && (function->BuiltIn() == BuiltInFunction::Stdin ||
                     function->BuiltIn() == BuiltInFunction::StdinLine)) {
        if (_inZero)
            Report(diagnostics::DiagnosticCode::InvalidLifecycle,
                   expression.Range(),
                   "zero cannot cause external side effects");
        if (_currentFunction) _forbiddenZeroEffects.insert(_currentFunction);
        if (argumentTypes.empty()) {
            Report(diagnostics::DiagnosticCode::WrongArgumentCount,
                   expression.Range(),
                   "function '" + function->Name() +
                       "' requires a destination reference");
            return function->ReturnType().WithoutConst();
        }
        for (std::size_t index = 0; index + 1 < argumentTypes.size(); ++index) {
            if (argumentTypes[index].IsError() ||
                argumentTypes[index].IsPrintable())
                continue;
            Report(diagnostics::DiagnosticCode::IncompatibleArgument,
                   expression.Arguments()[index]->Range(),
                   "function '" + function->Name() +
                       "' requires printable prompt values, found '" +
                       std::string(argumentTypes[index].Name()) + "'");
        }
        const SemanticType destination = argumentTypes.back();
        const bool reference =
            destination.Kind() == SemanticTypeKind::Reference;
        const SemanticType target =
            reference ? *destination.Target() : SemanticType::Error();
        const bool supported = target.IsInteger() || target.IsFloating() ||
                               target.Kind() == SemanticTypeKind::Bool ||
                               target.Kind() == SemanticTypeKind::Char ||
                               target.Kind() == SemanticTypeKind::String;
        if (!destination.IsError() &&
            (!reference || target.IsConst() || !supported)) {
            Report(diagnostics::DiagnosticCode::IncompatibleArgument,
                   expression.Arguments().back()->Range(),
                   "function '" + function->Name() +
                       "' requires a mutable reference to string, bool, char, "
                       "an integer, or a floating-point value");
        }
        return function->ReturnType().WithoutConst();
    }
    if (function &&
        (function->BuiltIn() == BuiltInFunction::Stdout ||
         function->BuiltIn() == BuiltInFunction::StdoutLine ||
         function->BuiltIn() == BuiltInFunction::StdoutFormat ||
         function->BuiltIn() == BuiltInFunction::Stderr ||
         function->BuiltIn() == BuiltInFunction::StderrLine ||
         function->BuiltIn() == BuiltInFunction::StderrFormat ||
         function->BuiltIn() == BuiltInFunction::StringFormat ||
         function->BuiltIn() == BuiltInFunction::StringFormatCrash)) {
        const bool stringFormat =
            function->BuiltIn() == BuiltInFunction::StringFormat ||
            function->BuiltIn() == BuiltInFunction::StringFormatCrash;
        if (_inZero && !stringFormat)
            Report(diagnostics::DiagnosticCode::InvalidLifecycle,
                   expression.Range(),
                   "zero cannot cause external side effects");
        if (_currentFunction) _forbiddenZeroEffects.insert(_currentFunction);
        const bool formatted =
            function->BuiltIn() == BuiltInFunction::StdoutFormat ||
            function->BuiltIn() == BuiltInFunction::StderrFormat ||
            stringFormat;
        if (!formatted) {
            for (std::size_t index = 0; index < argumentTypes.size(); ++index) {
                if (argumentTypes[index].IsError() ||
                    argumentTypes[index].IsPrintable())
                    continue;
                Report(diagnostics::DiagnosticCode::IncompatibleArgument,
                       expression.Arguments()[index]->Range(),
                       "function '" + function->Name() +
                           "' requires printable values, found '" +
                           std::string(argumentTypes[index].Name()) + "'");
            }
            return function->ReturnType().WithoutConst();
        }

        if (expression.Arguments().empty()) {
            Report(
                diagnostics::DiagnosticCode::WrongArgumentCount,
                expression.Range(),
                "function '" + function->Name() + "' requires a format string");
            return function->ReturnType().WithoutConst();
        }

        const core::Expression* formatExpression =
            expression.Arguments().front().get();
        while (formatExpression->Kind() ==
               core::AstNodeKind::ParenthesizedExpression)
            formatExpression =
                &static_cast<const core::ParenthesizedExpression&>(
                     *formatExpression)
                     .Inner();
        if (formatExpression->Kind() != core::AstNodeKind::LiteralExpression ||
            static_cast<const core::LiteralExpression&>(*formatExpression)
                    .LiteralType() != TokenType::StringLiteral) {
            Report(diagnostics::DiagnosticCode::InvalidFormat,
                   expression.Arguments().front()->Range(),
                   function->Name() + " format must be a string literal");
            return function->ReturnType().WithoutConst();
        }

        const auto& literal =
            static_cast<const core::LiteralExpression&>(*formatExpression);
        const core::OutputFormat format =
            core::ParseOutputFormat(literal.Value());
        if (!format.IsValid()) {
            Report(diagnostics::DiagnosticCode::InvalidFormat,
                   expression.Arguments().front()->Range(), format.error);
            return function->ReturnType().WithoutConst();
        }
        if (argumentTypes.size() != format.ArgumentCount() + 1) {
            Report(diagnostics::DiagnosticCode::WrongArgumentCount,
                   expression.Range(),
                   "format requires " + std::to_string(format.ArgumentCount()) +
                       " value argument(s), found " +
                       std::to_string(argumentTypes.size() - 1));
        }

        std::size_t argumentIndex = 1;
        for (const auto& part : format.parts) {
            if (part.kind == core::OutputFormatPartKind::Literal) continue;
            if (argumentIndex >= argumentTypes.size()) break;
            const SemanticType type = argumentTypes[argumentIndex];
            if (!type.IsError() && !type.IsPrintable())
                Report(diagnostics::DiagnosticCode::IncompatibleArgument,
                       expression.Arguments()[argumentIndex]->Range(),
                       "format argument " + std::to_string(argumentIndex) +
                           " for '{}' requires a printable value, found '" +
                           std::string(type.Name()) + "'");
            ++argumentIndex;
        }
        return function->ReturnType().WithoutConst();
    }
    if (argumentTypes.size() != signature->parameters.size()) {
        Report(diagnostics::DiagnosticCode::WrongArgumentCount,
               expression.Range(),
               std::string(function ? "function '" + function->Name() + "'"
                                    : "callable") +
                   (minimumArguments &&
                            *minimumArguments != signature->parameters.size()
                        ? " expects between " +
                              std::to_string(*minimumArguments) + " and " +
                              std::to_string(signature->parameters.size())
                        : " expects " +
                              std::to_string(signature->parameters.size())) +
                   " argument(s), found " +
                   std::to_string(expression.Arguments().size()));
    }
    const std::size_t count =
        argumentTypes.size() < signature->parameters.size()
            ? argumentTypes.size()
            : signature->parameters.size();
    for (std::size_t index = 0; index < count; ++index) {
        const SemanticType expected = signature->parameters[index];
        const bool assignable =
            function && function->IsExtern()
                ? IsExternAssignable(expected, argumentTypes[index])
                : IsAssignable(expected, argumentTypes[index]);
        if (!assignable && !CanReborrow(expected, argumentTypes[index])) {
            Report(diagnostics::DiagnosticCode::IncompatibleArgument,
                   arguments[index]->Range(),
                   "argument " + std::to_string(index + 1) + ": " +
                       TypeMismatch(signature->parameters[index],
                                    argumentTypes[index]));
        }
    }
    return signature->result.WithoutConst();
}

namespace {

bool IsStringViewIndex(SemanticType type) {
    return type.Kind() == SemanticTypeKind::I32 ||
           type.Kind() == SemanticTypeKind::U32;
}

bool IsOverflowingArithmeticMember(const std::string& member) {
    return member == "overflow_add" || member == "overflow_sub" ||
           member == "overflow_mul";
}

}  // namespace

SemanticType SemanticAnalyzer::AnalyzeMemberCall(
    const core::MemberCallExpression& expression) {
    if (expression.Access() == TokenType::Scope)
        return AnalyzeMethodCall(expression, SemanticType::Error());
    const SemanticType receiver = AnalyzeExpression(expression.Receiver());
    if (expression.Member() == "$range_index") {
        if (expression.Arguments().size() != 1) {
            Report(diagnostics::DiagnosticCode::WrongArgumentCount,
                   expression.Range(),
                   "internal range index expects one cursor argument");
            for (const auto& argument : expression.Arguments())
                AnalyzeExpression(*argument);
            return SemanticType::Error();
        }
        const SemanticType cursor = AnalyzeExpression(
            *expression.Arguments()[0], SemanticType(SemanticTypeKind::USize));
        if (!cursor.IsError() && cursor.Kind() != SemanticTypeKind::USize) {
            Report(diagnostics::DiagnosticCode::IncompatibleArgument,
                   expression.Arguments()[0]->Range(),
                   "internal range cursor must have type 'usize'");
        }
        if (receiver.Kind() == SemanticTypeKind::StringView ||
            receiver.Kind() == SemanticTypeKind::String)
            return SemanticType(SemanticTypeKind::U32);
        if (receiver.IsSequence()) return SemanticType(SemanticTypeKind::USize);
        return SemanticType::Error();
    }
    if (receiver.Kind() == SemanticTypeKind::Structure ||
        receiver.Kind() == SemanticTypeKind::Enum ||
        receiver.Kind() == SemanticTypeKind::Interface ||
        ((receiver.Kind() == SemanticTypeKind::Pointer ||
          receiver.Kind() == SemanticTypeKind::Reference) &&
         (receiver.Target()->Kind() == SemanticTypeKind::Structure ||
          receiver.Target()->Kind() == SemanticTypeKind::Enum ||
          receiver.Target()->Kind() == SemanticTypeKind::Interface)))
        return AnalyzeMethodCall(expression, receiver);
    if (expression.Access() != TokenType::Dot)
        Report(diagnostics::DiagnosticCode::InvalidMember, expression.Range(),
               "this member requires '.'");
    if (receiver.IsInteger() &&
        IsOverflowingArithmeticMember(expression.Member())) {
        const SemanticType valueType = receiver.WithoutConst();
        std::vector<SemanticType> arguments;
        arguments.reserve(expression.Arguments().size());
        for (const auto& argument : expression.Arguments())
            arguments.push_back(AnalyzeExpression(*argument, valueType));
        if (arguments.size() != 1) {
            Report(diagnostics::DiagnosticCode::WrongArgumentCount,
                   expression.Range(),
                   "member '" + expression.Member() +
                       "' expects 1 argument, found " +
                       std::to_string(arguments.size()));
        } else if (!arguments[0].IsError() &&
                   !(arguments[0].WithoutConst() == valueType)) {
            Report(diagnostics::DiagnosticCode::IncompatibleArgument,
                   expression.Arguments()[0]->Range(),
                   "overflowing arithmetic requires matching integer types; "
                   "expected '" +
                       valueType.Name() + "', found '" + arguments[0].Name() +
                       "'");
        }
        return SemanticType(std::vector<SemanticType>{
            valueType, SemanticType(SemanticTypeKind::Bool)});
    }
    std::vector<SemanticType> arguments;
    arguments.reserve(expression.Arguments().size());
    for (const std::unique_ptr<core::Expression>& argument :
         expression.Arguments()) {
        arguments.push_back(AnalyzeExpression(*argument));
    }
    if (receiver.IsError()) return SemanticType::Error();
    if (receiver.IsSequence() &&
        (expression.Member() == "front" || expression.Member() == "back")) {
        if (receiver.Kind() == SemanticTypeKind::OwnedBuffer)
            RequireUnsafe(expression.Range());
        if (!arguments.empty())
            Report(diagnostics::DiagnosticCode::WrongArgumentCount,
                   expression.Range(), "front/back expect no arguments");
        if (receiver.Kind() == SemanticTypeKind::Array &&
            receiver.Length() == 0)
            Report(diagnostics::DiagnosticCode::ArrayIndexOutOfBounds,
                   expression.Range(),
                   "an empty array has no front or back element");
        return receiver.Target()->WithoutConst();
    }
    if (receiver.IsSequence() && expression.Member() == "len") {
        if (receiver.Kind() == SemanticTypeKind::OwnedBuffer)
            RequireUnsafe(expression.Range());
        if (!arguments.empty())
            Report(diagnostics::DiagnosticCode::WrongArgumentCount,
                   expression.Range(), "array/slice len expects no arguments");
        return SemanticType(SemanticTypeKind::USize);
    }
    if ((receiver.Kind() == SemanticTypeKind::Slice ||
         receiver.Kind() == SemanticTypeKind::OwnedSlice) &&
        expression.Member() == "data") {
        RequireUnsafe(expression.Range());
        if (!arguments.empty())
            Report(diagnostics::DiagnosticCode::WrongArgumentCount,
                   expression.Range(), "slice data expects no arguments");
        return SemanticType(SemanticTypeKind::Pointer, *receiver.Target());
    }
    const bool isView = receiver.Kind() == SemanticTypeKind::StringView;
    const bool isString = receiver.Kind() == SemanticTypeKind::String;
    const bool scalarToString = receiver.IsNumeric() ||
                                receiver.Kind() == SemanticTypeKind::Bool ||
                                receiver.Kind() == SemanticTypeKind::Char;
    if (!isView && !isString &&
        !(scalarToString && expression.Member() == "string")) {
        Report(diagnostics::DiagnosticCode::InvalidMember,
               expression.MemberRange(),
               "type '" + std::string(receiver.Name()) + "' has no member '" +
                   expression.Member() + "'");
        return SemanticType::Error();
    }

    std::size_t expectedCount = 0;
    SemanticType result = SemanticType::Error();
    if (expression.Member() == "string") {
        result = SemanticType(SemanticTypeKind::String);
    } else if (expression.Member() == "view" && isString) {
        result = SemanticType(SemanticTypeKind::StringView);
    } else if (expression.Member() == "copy" && isString) {
        result = SemanticType(SemanticTypeKind::String);
    } else if (expression.Member() == "data" && (isView || isString)) {
        RequireUnsafe(expression.Range());
        result = SemanticType(SemanticTypeKind::Pointer,
                              SemanticType(SemanticTypeKind::Char, true));
    } else if (expression.Member() == "len" && (isView || isString)) {
        result = SemanticType(SemanticTypeKind::U32);
    } else if (expression.Member() == "at" && (isView || isString)) {
        expectedCount = 1;
        result = SemanticType(SemanticTypeKind::Char);
    } else if (expression.Member() == "substr" && (isView || isString)) {
        expectedCount = 2;
        result = SemanticType(SemanticTypeKind::StringView);
    } else {
        Report(diagnostics::DiagnosticCode::InvalidMember,
               expression.MemberRange(),
               "type '" + std::string(receiver.Name()) + "' has no member '" +
                   expression.Member() + "'");
        return SemanticType::Error();
    }

    if (arguments.size() != expectedCount) {
        Report(diagnostics::DiagnosticCode::WrongArgumentCount,
               expression.Range(),
               "member '" + expression.Member() + "' expects " +
                   std::to_string(expectedCount) + " argument(s), found " +
                   std::to_string(arguments.size()));
        return result;
    }
    if (_currentFunction && result.Kind() == SemanticTypeKind::String)
        _forbiddenZeroEffects.insert(_currentFunction);
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        if (!arguments[index].IsError() &&
            !IsStringViewIndex(arguments[index])) {
            Report(diagnostics::DiagnosticCode::IncompatibleArgument,
                   expression.Arguments()[index]->Range(),
                   "string index must have type 'i32' or 'u32', found '" +
                       std::string(arguments[index].Name()) + "'");
        }
    }
    return result;
}

SemanticType SemanticAnalyzer::AnalyzeSlice(
    const core::SliceExpression& expression, SemanticType expected) {
    const SemanticType receiver = AnalyzeExpression(expression.Receiver());
    for (const core::Expression* bound :
         {expression.Start(), expression.End()}) {
        if (bound == nullptr) continue;
        const SemanticType type =
            AnalyzeExpression(*bound, SemanticType(SemanticTypeKind::USize));
        if (!type.IsError() && type.Kind() != SemanticTypeKind::USize)
            Report(diagnostics::DiagnosticCode::IncompatibleType,
                   bound->Range(), "slice bounds must have type 'usize'");
    }
    if (receiver.IsError()) return receiver;
    const bool heap = receiver.Kind() == SemanticTypeKind::Pointer ||
                      receiver.Kind() == SemanticTypeKind::OwnedBuffer;
    if (heap) {
        RequireUnsafe(expression.Range());
        if (!StorageSize(*receiver.Target()))
            Report(diagnostics::DiagnosticCode::InvalidAllocation,
                   expression.Range(),
                   "raw slicing requires a pointer to a sized value type");
    }
    if (!receiver.IsSequence() && !heap) {
        Report(diagnostics::DiagnosticCode::NotIndexable,
               expression.Receiver().Range(),
               "slicing requires a fixed array, slice, or raw pointer");
        return SemanticType::Error();
    }
    SemanticType element = *receiver.Target();
    if (receiver.Kind() == SemanticTypeKind::Array) {
        if (!IsSafePlace(expression.Receiver())) {
            Report(diagnostics::DiagnosticCode::InvalidReference,
                   expression.Receiver().Range(),
                   "a slice requires initialized safe storage, not an array "
                   "temporary or raw pointee");
            return SemanticType::Error();
        }
        if (PlaceType(expression.Receiver()).IsConst())
            element = element.WithConst();
    }
    if (expected.Kind() == SemanticTypeKind::Slice &&
        expected.Target()->IsConst())
        element = element.WithConst();
    const std::optional<std::uint64_t> start =
        expression.Start() == nullptr ? std::optional<std::uint64_t>(0)
                                      : ConstantIndex(*expression.Start());
    const std::optional<std::uint64_t> end =
        expression.End() == nullptr
            ? (receiver.Kind() == SemanticTypeKind::Array
                   ? std::optional<std::uint64_t>(receiver.Length())
                   : std::nullopt)
            : ConstantIndex(*expression.End());
    if ((start && end && *start > *end) ||
        (receiver.Kind() == SemanticTypeKind::Array &&
         ((start && *start > receiver.Length()) ||
          (end && *end > receiver.Length()))))
        Report(diagnostics::DiagnosticCode::SliceBounds, expression.Range(),
               "slice bounds must satisfy 0 <= start <= end <= length");
    return SemanticType(SemanticTypeKind::Slice, element);
}

SemanticType SemanticAnalyzer::AnalyzeIndex(
    const core::IndexExpression& expression) {
    const SemanticType receiver = AnalyzeExpression(expression.Receiver());
    const bool heap = receiver.Kind() == SemanticTypeKind::Pointer ||
                      receiver.Kind() == SemanticTypeKind::OwnedBuffer;
    if (heap) {
        RequireUnsafe(expression.Range());
        if (!StorageSize(*receiver.Target()))
            Report(diagnostics::DiagnosticCode::InvalidAllocation,
                   expression.Range(),
                   "raw indexing requires a pointer to a sized value type");
    }
    const SemanticType index = AnalyzeExpression(
        expression.Index(), (receiver.IsSequence() || heap)
                                ? SemanticType(SemanticTypeKind::USize)
                                : SemanticType::Error());
    if ((receiver.IsSequence() || heap)) {
        if (!index.IsError() && index.Kind() != SemanticTypeKind::USize)
            Report(diagnostics::DiagnosticCode::IncompatibleArgument,
                   expression.Index().Range(),
                   "array/slice index must have type 'usize'");
        const std::optional<std::uint64_t> constant =
            ConstantIndex(expression.Index());
        if (receiver.Kind() == SemanticTypeKind::Array &&
            !expression.IsRangeElement() &&
            (receiver.Length() == 0 ||
             (constant && *constant >= receiver.Length())))
            Report(diagnostics::DiagnosticCode::ArrayIndexOutOfBounds,
                   expression.Index().Range(),
                   "index is outside array length " +
                       std::to_string(receiver.Length()));
        return receiver.Target()->WithoutConst();
    }
    if (!receiver.IsError() &&
        receiver.Kind() != SemanticTypeKind::StringView &&
        receiver.Kind() != SemanticTypeKind::String) {
        Report(diagnostics::DiagnosticCode::NotIndexable,
               expression.Receiver().Range(),
               "type '" + std::string(receiver.Name()) + "' cannot be indexed");
        return SemanticType::Error();
    }
    if (!index.IsError() && !IsStringViewIndex(index)) {
        Report(diagnostics::DiagnosticCode::IncompatibleArgument,
               expression.Index().Range(),
               "string index must have type 'i32' or 'u32', found '" +
                   std::string(index.Name()) + "'");
    }
    return receiver.IsError() ? SemanticType::Error()
                              : SemanticType(SemanticTypeKind::Char);
}

const StructureType* SemanticAnalyzer::CurrentOwner() const {
    return _currentFunction ? _currentFunction->Owner() : nullptr;
}

const EnumType* SemanticAnalyzer::CurrentEnumOwner() const {
    return _currentFunction ? _currentFunction->EnumOwner() : nullptr;
}

const InterfaceType* SemanticAnalyzer::CurrentInterfaceOwner() const {
    return _currentFunction ? _currentFunction->InterfaceOwner() : nullptr;
}

void SemanticAnalyzer::RegisterStructures(const core::CompilationUnit& unit) {
    for (const auto& declaration : unit.Declarations()) {
        if (declaration->Kind() != core::AstNodeKind::StructureDeclaration)
            continue;
        const auto& syntax =
            static_cast<const core::StructureDeclaration&>(*declaration);
        SetSource(syntax.sourceId);
        auto structure = std::make_unique<StructureType>();
        structure->name = TopName(syntax, syntax.Name());
        structure->displayName = syntax.specializationName.empty()
                                     ? syntax.Name()
                                     : syntax.specializationName;
        structure->syntax = &syntax;
        AddSymbol(std::make_unique<Symbol>(
                      SymbolKind::StructureType, TopName(syntax, syntax.Name()),
                      SemanticType(structure.get()), syntax.NameRange(), false),
                  syntax);
        _model._structures.push_back(std::move(structure));
    }
}

void SemanticAnalyzer::ResolveNominalRelations() {
    const auto resolve = [&](const std::vector<core::NominalParent>& syntax,
                             std::vector<InterfaceReference>& result,
                             std::string_view relation,
                             const InterfaceType* owner = nullptr) {
        std::unordered_set<const InterfaceType*> seen;
        for (const auto& parent : syntax) {
            if (ReportAmbiguousImport(parent.name, parent.range)) continue;
            const Symbol* symbol = _currentScope->Lookup(parent.name);
            if (!symbol) {
                if (!ReportInaccessibleImport(parent.name, parent.range))
                    Report(diagnostics::DiagnosticCode::UnknownType,
                           parent.range,
                           "unknown interface '" + parent.name + "'");
                continue;
            }
            if (symbol->Kind() != SymbolKind::InterfaceType) {
                Report(diagnostics::DiagnosticCode::InvalidInterface,
                       parent.range,
                       "'" + parent.name + "' is not an interface");
                continue;
            }
            const InterfaceType* interfaceType = symbol->Type().Interface();
            if (interfaceType == owner) {
                Report(diagnostics::DiagnosticCode::InvalidInterface,
                       parent.range,
                       "interface '" + owner->displayName +
                           "' cannot inherit from itself");
                continue;
            }
            if (!seen.insert(interfaceType).second) {
                Report(diagnostics::DiagnosticCode::DuplicateDeclaration,
                       parent.range,
                       "duplicate interface " + std::string(relation) + " '" +
                           parent.name + "'");
                continue;
            }
            result.push_back({interfaceType, parent.range});
        }
    };

    for (const auto& interfaceType : _model._interfaces) {
        SetSource(interfaceType->syntax->sourceId);
        resolve(interfaceType->syntax->Parents(), interfaceType->parents,
                "parent", interfaceType.get());
    }
    for (const auto& structure : _model._structures) {
        SetSource(structure->syntax->sourceId);
        resolve(structure->syntax->Conformances(), structure->conformances,
                "conformance");
    }
    for (const auto& enumeration : _model._enums) {
        SetSource(enumeration->syntax->sourceId);
        resolve(enumeration->syntax->Conformances(), enumeration->conformances,
                "conformance");
    }

    enum class VisitState { Unvisited, Visiting, Visited };
    std::unordered_map<const InterfaceType*, VisitState> states;
    std::vector<const InterfaceType*> stack;
    std::unordered_set<const InterfaceType*> cyclic;
    std::function<void(const InterfaceType&)> visit =
        [&](const InterfaceType& interfaceType) {
            states[&interfaceType] = VisitState::Visiting;
            stack.push_back(&interfaceType);
            for (const auto& parent : interfaceType.parents) {
                const VisitState state = states[parent.type];
                if (state == VisitState::Visiting) {
                    Report(diagnostics::DiagnosticCode::InvalidInterface,
                           parent.range,
                           "interface inheritance cycle involving '" +
                               parent.type->displayName + "'");
                    const auto first =
                        std::find(stack.begin(), stack.end(), parent.type);
                    cyclic.insert(first, stack.end());
                    continue;
                }
                if (state == VisitState::Unvisited) visit(*parent.type);
            }
            stack.pop_back();
            states[&interfaceType] = VisitState::Visited;
        };
    for (const auto& interfaceType : _model._interfaces)
        if (states[interfaceType.get()] == VisitState::Unvisited)
            visit(*interfaceType);

    const auto closure = [](const InterfaceType& root) {
        std::vector<const InterfaceType*> result;
        std::unordered_set<const InterfaceType*> seen;
        std::unordered_set<const InterfaceType*> active{&root};
        const auto append = [&](const auto& self,
                                const InterfaceType& current) -> void {
            for (const auto& parent : current.parents) {
                if (active.contains(parent.type)) continue;
                if (seen.insert(parent.type).second)
                    result.push_back(parent.type);
                active.insert(parent.type);
                self(self, *parent.type);
                active.erase(parent.type);
            }
        };
        append(append, root);
        return result;
    };
    for (const auto& interfaceType : _model._interfaces)
        interfaceType->ancestors = closure(*interfaceType);

    const auto extends = [](const InterfaceType& type,
                            const InterfaceType& candidate) {
        return std::find(type.ancestors.begin(), type.ancestors.end(),
                         &candidate) != type.ancestors.end();
    };
    const auto validateComposition =
        [&](const std::vector<InterfaceReference>& direct,
            std::string_view declarationKind) {
            for (std::size_t rightIndex = 1; rightIndex < direct.size();
                 ++rightIndex) {
                const auto& right = direct[rightIndex];
                for (std::size_t leftIndex = 0; leftIndex < rightIndex;
                     ++leftIndex) {
                    const auto& left = direct[leftIndex];
                    if (cyclic.contains(left.type) ||
                        cyclic.contains(right.type))
                        continue;
                    if (extends(*left.type, *right.type) ||
                        extends(*right.type, *left.type)) {
                        const auto* derived = extends(*left.type, *right.type)
                                                  ? left.type
                                                  : right.type;
                        const auto* base =
                            derived == left.type ? right.type : left.type;
                        Report(diagnostics::DiagnosticCode::InvalidInterface,
                               right.range,
                               "interface '" + derived->displayName +
                                   "' already extends '" + base->displayName +
                                   "' in " + std::string(declarationKind) +
                                   " composition");
                        continue;
                    }
                    const InterfaceType* shared = nullptr;
                    for (const InterfaceType* ancestor : left.type->ancestors)
                        if (ancestor == right.type ||
                            std::find(right.type->ancestors.begin(),
                                      right.type->ancestors.end(), ancestor) !=
                                right.type->ancestors.end()) {
                            shared = ancestor;
                            break;
                        }
                    if (shared)
                        Report(diagnostics::DiagnosticCode::InvalidInterface,
                               right.range,
                               "interface composition has a diamond through '" +
                                   shared->displayName + "'");
                }
            }
        };

    for (const auto& interfaceType : _model._interfaces)
        validateComposition(interfaceType->parents, "interface");
    for (const auto& structure : _model._structures)
        validateComposition(structure->conformances, "structure");
    for (const auto& enumeration : _model._enums)
        validateComposition(enumeration->conformances, "enum");

    const auto conformanceClosure = [](const auto& direct) {
        std::vector<const InterfaceType*> result;
        std::unordered_set<const InterfaceType*> seen;
        for (const auto& reference : direct) {
            if (seen.insert(reference.type).second)
                result.push_back(reference.type);
            for (const InterfaceType* ancestor : reference.type->ancestors)
                if (seen.insert(ancestor).second) result.push_back(ancestor);
        }
        return result;
    };
    for (const auto& structure : _model._structures)
        structure->conformanceClosure =
            conformanceClosure(structure->conformances);
    for (const auto& enumeration : _model._enums)
        enumeration->conformanceClosure =
            conformanceClosure(enumeration->conformances);
}

void SemanticAnalyzer::ResolveInterfaceMethods() {
    const auto resolveContract = [&](const InterfaceType& owner,
                                     const core::InterfaceMethod& method) {
        const core::FunctionDeclaration& function = method.Signature();
        SetSource(function.sourceId);

        if (!function.genericParameters.empty())
            Report(diagnostics::DiagnosticCode::InvalidInterface,
                   function.NameRange(),
                   "interface methods cannot declare generic parameters");
        if (function.Name() == "init" || function.Name() == "zero" ||
            function.Name() == "tear")
            Report(diagnostics::DiagnosticCode::InvalidInterface,
                   function.NameRange(),
                   "interface methods cannot use lifecycle names");
        if (function.HasModifier(core::FunctionModifier::Private))
            Report(diagnostics::DiagnosticCode::InvalidInterface,
                   function.NameRange(), "interface methods cannot be private");

        std::vector<SemanticType> parameterTypes;
        std::vector<core::OwnershipContract> parameterContracts;
        bool receiver = false;
        std::unordered_set<std::string> names;
        for (std::size_t index = 0; index < function.Parameters().size();
             ++index) {
            const auto& parameter = function.Parameters()[index];
            const SemanticType type = ResolveType(parameter->Type());
            if (!names.insert(parameter->Name()).second)
                Report(diagnostics::DiagnosticCode::DuplicateDeclaration,
                       parameter->NameRange(),
                       "duplicate interface method parameter '" +
                           parameter->Name() + "'");
            if (parameter->DefaultValue() != nullptr)
                Report(diagnostics::DiagnosticCode::InvalidInterface,
                       parameter->DefaultValue()->Range(),
                       "interface method parameters cannot have default "
                       "arguments");
            if (parameter->Name() == "self") {
                const bool valid =
                    index == 0 && type.Kind() == SemanticTypeKind::Reference &&
                    type.Target()->Kind() == SemanticTypeKind::Interface &&
                    type.Target()->Interface() == &owner;
                if (!valid)
                    Report(diagnostics::DiagnosticCode::InvalidInterface,
                           parameter->Range(),
                           "interface receiver must be the first parameter "
                           "and spelled '&self' or '&const self'");
                receiver = valid;
            }
            parameterTypes.push_back(type);
            parameterContracts.push_back(parameter->Type().Contract());
        }
        if (!receiver)
            Report(diagnostics::DiagnosticCode::InvalidInterface,
                   function.NameRange(),
                   "interface methods require an '&self' or '&const self' "
                   "receiver");

        const SemanticType returnType =
            ResolveType(function.ReturnType(), true);
        std::vector<std::size_t> borrowSources;
        if (RequiresBorrowSource(returnType) && !function.HasBorrowSources())
            Report(diagnostics::DiagnosticCode::InvalidBorrowContract,
                   function.ReturnType().Range(),
                   "reference and slice results require a '<- source>' "
                   "borrow contract");
        else if (!CanCarryBorrow(returnType) && function.HasBorrowSources())
            Report(diagnostics::DiagnosticCode::InvalidBorrowContract,
                   function.BorrowSourceRanges().empty()
                       ? function.VariadicPackRange()
                       : function.BorrowSourceRanges().front(),
                   "a borrow contract requires a result that can carry a "
                   "borrow");
        else {
            for (std::size_t source = 0;
                 source < function.BorrowSources().size(); ++source) {
                const auto found = std::find_if(
                    function.Parameters().begin(), function.Parameters().end(),
                    [&](const auto& parameter) {
                        return parameter->Name() ==
                               function.BorrowSources()[source];
                    });
                if (found == function.Parameters().end()) {
                    Report(diagnostics::DiagnosticCode::InvalidBorrowContract,
                           function.BorrowSourceRanges()[source],
                           "borrow source '" +
                               function.BorrowSources()[source] +
                               "' is not a parameter");
                    continue;
                }
                const std::size_t index = static_cast<std::size_t>(
                    found - function.Parameters().begin());
                if (!CanCarryBorrow(parameterTypes[index])) {
                    Report(diagnostics::DiagnosticCode::InvalidBorrowContract,
                           function.BorrowSourceRanges()[source],
                           "borrow source '" +
                               function.BorrowSources()[source] +
                               "' cannot carry a borrow");
                    continue;
                }
                borrowSources.push_back(index);
            }
        }

        CallableSignature signature{
            returnType,
            std::move(parameterTypes),
            std::move(parameterContracts),
            ResultContracts(function.ReturnType()),
            std::move(borrowSources),
            function.IsUnsafe(),
            function.HasModifier(core::FunctionModifier::Once),
            function.IsCrashable()};
        auto callableSignature =
            std::make_shared<const CallableSignature>(signature);
        MethodContract contract = NormalizeMethodContract(
            function.Name(), std::move(signature), receiver,
            function.HasVariadicPack(), function.BorrowsVariadicPack(),
            function.HasModifier(core::FunctionModifier::Protected)
                ? MethodVisibility::Protected
                : MethodVisibility::Public,
            method.Category());
        contract.declaredBy = &owner;
        contract.syntax = &method;
        auto callable = std::make_unique<FunctionSymbol>(
            owner.name +
                "::" + EffectName(function.Name(), function.IsCrashable()),
            std::move(callableSignature), function.NameRange(),
            BuiltInFunction::None, nullptr, false, true, false, "", nullptr,
            &function, function.BorrowsVariadicPack(), &owner,
            function.HasModifier(core::FunctionModifier::Protected));
        contract.callable = callable.get();
        _model._declarations.emplace(&function, contract.callable);
        _model._symbols.push_back(std::move(callable));
        return contract;
    };

    std::unordered_set<const InterfaceType*> resolving;
    std::unordered_set<const InterfaceType*> resolved;
    const auto resolve = [&](const auto& self,
                             InterfaceType& interfaceType) -> void {
        if (resolved.contains(&interfaceType) ||
            !resolving.insert(&interfaceType).second)
            return;
        for (const auto& parent : interfaceType.parents)
            self(self, *const_cast<InterfaceType*>(parent.type));

        for (const auto& parent : interfaceType.parents) {
            for (const MethodContract* inherited : parent.type->methods) {
                const auto collision = std::find_if(
                    interfaceType.methods.begin(), interfaceType.methods.end(),
                    [&](const MethodContract* candidate) {
                        return candidate->key == inherited->key;
                    });
                if (collision != interfaceType.methods.end()) {
                    Report(diagnostics::DiagnosticCode::InvalidInterface,
                           parent.range,
                           "interface composition has conflicting method '" +
                               inherited->key.name + "' from '" +
                               (*collision)->declaredBy->displayName +
                               "' and '" + parent.type->displayName + "'");
                    continue;
                }
                interfaceType.methods.push_back(inherited);
            }
        }

        interfaceType.declaredMethods.reserve(
            interfaceType.syntax->Methods().size());
        for (const auto& syntax : interfaceType.syntax->Methods()) {
            MethodContract contract = resolveContract(interfaceType, *syntax);
            const auto inherited = std::find_if(
                interfaceType.methods.begin(), interfaceType.methods.end(),
                [&](const MethodContract* candidate) {
                    return candidate->key == contract.key;
                });
            if (inherited != interfaceType.methods.end()) {
                contract.inherited = *inherited;
                if ((*inherited)->declaredBy == &interfaceType) {
                    Report(diagnostics::DiagnosticCode::DuplicateDeclaration,
                           syntax->Signature().NameRange(),
                           "duplicate interface method overload '" +
                               contract.key.name + "'");
                } else {
                    if (!contract.SameContract(**inherited))
                        Report(diagnostics::DiagnosticCode::InvalidInterface,
                               syntax->Signature().NameRange(),
                               "method '" + contract.key.name +
                                   "' changes the inherited contract from '" +
                                   (*inherited)->declaredBy->displayName + "'");
                    if ((*inherited)->category == core::MethodCategory::Final)
                        Report(diagnostics::DiagnosticCode::InvalidInterface,
                               syntax->CategoryRange(),
                               "final method '" + contract.key.name +
                                   "' cannot be redeclared");
                    if ((*inherited)->category ==
                            core::MethodCategory::Default &&
                        contract.category == core::MethodCategory::Virtual)
                        Report(diagnostics::DiagnosticCode::InvalidInterface,
                               syntax->CategoryRange(),
                               "default method '" + contract.key.name +
                                   "' cannot become virtual again");
                }
            }

            interfaceType.declaredMethods.push_back(std::move(contract));
            const MethodContract* declared =
                &interfaceType.declaredMethods.back();
            if (inherited == interfaceType.methods.end())
                interfaceType.methods.push_back(declared);
            else
                *inherited = declared;
        }
        resolving.erase(&interfaceType);
        resolved.insert(&interfaceType);
    };

    for (const auto& interfaceType : _model._interfaces)
        resolve(resolve, *interfaceType);

    const auto validateConcreteComposition =
        [&](const std::vector<InterfaceReference>& conformances,
            std::string_view declarationKind) {
            std::vector<const MethodContract*> methods;
            for (const auto& conformance : conformances)
                for (const MethodContract* method : conformance.type->methods) {
                    const auto collision =
                        std::find_if(methods.begin(), methods.end(),
                                     [&](const MethodContract* candidate) {
                                         return candidate->key == method->key;
                                     });
                    if (collision != methods.end()) {
                        Report(
                            diagnostics::DiagnosticCode::InvalidInterface,
                            conformance.range,
                            "interface composition has conflicting method '" +
                                method->key.name + "' in " +
                                std::string(declarationKind));
                        continue;
                    }
                    methods.push_back(method);
                }
        };
    for (const auto& structure : _model._structures)
        validateConcreteComposition(structure->conformances, "structure");
    for (const auto& enumeration : _model._enums)
        validateConcreteComposition(enumeration->conformances, "enum");
}

void SemanticAnalyzer::ValidateInterfaceConformance() {
    for (const auto& interfaceType : _model._interfaces)
        for (const MethodContract& contract : interfaceType->declaredMethods) {
            const bool requiresBody =
                contract.category == core::MethodCategory::Default ||
                contract.category == core::MethodCategory::Final;
            if (requiresBody && contract.provider == nullptr)
                Report(diagnostics::DiagnosticCode::InvalidInterface,
                       contract.syntax->CategoryRange(),
                       (contract.category == core::MethodCategory::Default
                            ? "default"
                            : "final") +
                           std::string(" interface method '") +
                           contract.key.name +
                           "' requires exactly one out-of-line body");
        }

    const auto validate = [&](const auto& concrete,
                              std::string_view concreteKind) {
        std::vector<const MethodContract*> seen;
        for (const auto& conformance : concrete.conformances) {
            for (const MethodContract* required : conformance.type->methods) {
                if (std::ranges::any_of(
                        seen, [&](const MethodContract* existing) {
                            return existing->key == required->key;
                        }))
                    continue;
                seen.push_back(required);

                bool implemented = false;
                const auto methods = concrete.methods.find(
                    EffectName(required->key.name, required->isCrashable));
                if (methods != concrete.methods.end())
                    for (const FunctionSymbol* method : methods->second) {
                        const MethodContract candidate = MethodContractOf(
                            *method, method->IsProtected()
                                         ? MethodVisibility::Protected
                                         : MethodVisibility::Public);
                        if (candidate.key == required->key &&
                            !method->IsPrivate() &&
                            candidate.SameContract(*required)) {
                            implemented = true;
                            break;
                        }
                    }
                if (!implemented &&
                    required->category == core::MethodCategory::Virtual)
                    Report(diagnostics::DiagnosticCode::InvalidInterface,
                           conformance.range,
                           std::string(concreteKind) + " '" +
                               concrete.displayName + "' does not implement '" +
                               required->key.name +
                               "' required by interface '" +
                               required->declaredBy->displayName + "'");
            }
        }
    };

    for (const auto& structure : _model._structures)
        validate(*structure, "structure");
    for (const auto& enumeration : _model._enums)
        validate(*enumeration, "enum");
}

void SemanticAnalyzer::ResolveStructureFields() {
    for (const auto& structure : _model._structures) {
        SetSource(structure->syntax->sourceId);
        std::unordered_set<std::string> names;
        for (const auto& field : structure->syntax->Fields()) {
            if (!names.insert(field->Name()).second)
                Report(diagnostics::DiagnosticCode::DuplicateDeclaration,
                       field->NameRange(),
                       "duplicate field '" + field->Name() + "'");
            if (field->Name() == "init" || field->Name() == "zero" ||
                field->Name() == "tear")
                Report(diagnostics::DiagnosticCode::InvalidLifecycle,
                       field->NameRange(),
                       "init, zero, and tear are reserved lifecycle names");
            SemanticType type = ResolveTypeSyntax(field->Type());
            structure->fields.push_back({field->Name(), type, field.get()});
        }
    }
    std::unordered_set<const StructureType*> visiting;
    std::function<bool(SemanticType)> validate = [&](SemanticType type) {
        if (type.Kind() == SemanticTypeKind::Pointer ||
            type.Kind() == SemanticTypeKind::Reference ||
            type.Kind() == SemanticTypeKind::OwnedBuffer ||
            type.Kind() == SemanticTypeKind::OwnedSlice ||
            type.Kind() == SemanticTypeKind::Slice)
            return true;
        if (type.Kind() == SemanticTypeKind::Structure) {
            auto* structure = const_cast<StructureType*>(type.Structure());
            if (structure->validLayout) return true;
            if (!visiting.insert(structure).second) return false;
            bool valid = true;
            for (const auto& field : structure->fields) {
                if (!validate(field.type)) {
                    Report(diagnostics::DiagnosticCode::InvalidStructure,
                           field.syntax->Range(),
                           "field has recursive or unsupported storage; use a "
                           "pointer for recursive structures");
                    valid = false;
                }
            }
            visiting.erase(structure);
            structure->validLayout = valid;
            return valid;
        }
        if (type.Kind() == SemanticTypeKind::Array)
            return IsArrayElement(*type.Target()) && validate(*type.Target());
        if (type.Kind() == SemanticTypeKind::Tuple) {
            for (const auto& element : type.Elements())
                if (!validate(element)) return false;
            return true;
        }
        return type.IsNumeric() || type.IsErrorValue() ||
               type.Kind() == SemanticTypeKind::Enum ||
               type.Kind() == SemanticTypeKind::Bool ||
               type.Kind() == SemanticTypeKind::Char ||
               type.Kind() == SemanticTypeKind::String ||
               type.Kind() == SemanticTypeKind::Function || type.IsError();
    };
    for (const auto& structure : _model._structures) {
        if (validate(SemanticType(structure.get())) &&
            !StorageSize(SemanticType(structure.get())))
            Report(diagnostics::DiagnosticCode::InvalidStructure,
                   structure->syntax->Range(),
                   "structure layout exceeds supported size");
    }
    // Layouts must be complete before applying the ordinary compound-type
    // checks, including unsupported types hidden behind a pointer field.
    for (const auto& structure : _model._structures) {
        SetSource(structure->syntax->sourceId);
        for (const auto& field : structure->fields) {
            if (field.syntax->Initializer() && field.type.ContainsReference())
                Report(diagnostics::DiagnosticCode::InvalidStructure,
                       field.syntax->Initializer()->Range(),
                       "reference-bearing fields must be initialized in "
                       "build from an explicit borrow source");
            (void)ResolveType(field.syntax->Type());
        }
    }
}

void SemanticAnalyzer::GenerateInitializers() {
    std::function<bool(SemanticType)> unsafeDefault = [&](SemanticType type) {
        if (type.Kind() == SemanticTypeKind::Structure) {
            if (!type.Structure()->validLayout) return false;
            if (type.IsImmovable()) return true;
            if (type.Structure()->init)
                return type.Structure()->init->IsUnsafe();
            for (const auto& field : type.Structure()->fields)
                if (!field.syntax->Initializer() && unsafeDefault(field.type))
                    return true;
        }
        if (type.Kind() == SemanticTypeKind::Array)
            return unsafeDefault(*type.Target());
        if (type.Kind() == SemanticTypeKind::Tuple)
            for (const auto& element : type.Elements())
                if (unsafeDefault(element)) return true;
        return false;
    };
    for (const auto& structure : _model._structures) {
        if (structure->init ||
            SemanticType(structure.get()).ContainsReference())
            continue;
        const auto range = structure->syntax->Range();
        std::vector<std::unique_ptr<core::Statement>> statements;
        auto body = std::make_unique<core::BlockStatement>(
            range, std::vector<std::unique_ptr<core::Statement>>{});
        auto build = std::make_unique<core::BuildExpression>(
            range, std::make_unique<core::TypeNode>(range, structure->name),
            std::move(body));
        statements.push_back(
            std::make_unique<core::ReturnStatement>(range, std::move(build)));
        std::vector<core::FunctionModifier> modifiers;
        if (unsafeDefault(SemanticType(structure.get())))
            modifiers.push_back(core::FunctionModifier::Unsafe);
        auto init = std::make_unique<core::FunctionDeclaration>(
            range, std::move(modifiers),
            std::make_unique<core::TypeNode>(range, structure->name), "init",
            std::vector<std::unique_ptr<core::Parameter>>{},
            std::make_unique<core::BlockStatement>(range,
                                                   std::move(statements)),
            range, structure->name);
        init->moduleId = structure->syntax->moduleId;
        init->sourceId = structure->syntax->sourceId;
        RegisterFunction(*init, true);
        _model._generatedFunctions.push_back(std::move(init));
    }

    std::unordered_map<const StructureType*, bool> zeroable;
    std::unordered_set<const StructureType*> checking;
    std::function<bool(SemanticType)> canGenerateZero =
        [&](SemanticType type) -> bool {
        if (type.Kind() == SemanticTypeKind::Array)
            return canGenerateZero(*type.Target());
        if (type.Kind() == SemanticTypeKind::Tuple) {
            for (const auto& element : type.Elements())
                if (!canGenerateZero(element)) return false;
            return true;
        }
        if (type.Kind() == SemanticTypeKind::Enum) return type.IsZeroable();
        if (type.Kind() != SemanticTypeKind::Structure)
            return type.IsNumeric() || type.Kind() == SemanticTypeKind::Bool ||
                   type.Kind() == SemanticTypeKind::Char ||
                   type.Kind() == SemanticTypeKind::String ||
                   type.Kind() == SemanticTypeKind::Pointer ||
                   type.Kind() == SemanticTypeKind::OwnedBuffer ||
                   type.Kind() == SemanticTypeKind::OwnedSlice;
        const StructureType* nested = type.Structure();
        if (nested->zero) return true;
        if (const auto found = zeroable.find(nested); found != zeroable.end())
            return found->second;
        if (!checking.insert(nested).second) return false;
        bool result = true;
        for (const auto& field : nested->fields)
            if (!canGenerateZero(field.type)) {
                result = false;
                break;
            }
        checking.erase(nested);
        zeroable.emplace(nested, result);
        return result;
    };
    for (const auto& structure : _model._structures)
        (void)canGenerateZero(SemanticType(structure.get()));
    for (const auto& structure : _model._structures) {
        if (structure->zero || !zeroable[structure.get()]) continue;
        const auto range = structure->syntax->Range();
        std::vector<std::unique_ptr<core::Statement>> statements;
        auto body = std::make_unique<core::BlockStatement>(
            range, std::vector<std::unique_ptr<core::Statement>>{});
        auto build = std::make_unique<core::BuildExpression>(
            range, std::make_unique<core::TypeNode>(range, structure->name),
            std::move(body));
        statements.push_back(
            std::make_unique<core::ReturnStatement>(range, std::move(build)));
        auto zero = std::make_unique<core::FunctionDeclaration>(
            range, std::vector<core::FunctionModifier>{},
            std::make_unique<core::TypeNode>(range, structure->name), "zero",
            std::vector<std::unique_ptr<core::Parameter>>{},
            std::make_unique<core::BlockStatement>(range,
                                                   std::move(statements)),
            range, structure->name);
        zero->moduleId = structure->syntax->moduleId;
        zero->sourceId = structure->syntax->sourceId;
        RegisterFunction(*zero, true);
        _model._generatedFunctions.push_back(std::move(zero));
    }
}

void SemanticAnalyzer::GenerateJsonMethods() {
    const auto named = [](source::SourceRange range, std::string name) {
        return std::make_unique<core::NameExpression>(range, std::move(name));
    };
    const auto memberCall =
        [&](source::SourceRange range,
            std::unique_ptr<core::Expression> receiver, std::string member,
            std::vector<std::unique_ptr<core::Expression>> arguments = {}) {
            return std::make_unique<core::MemberCallExpression>(
                range, std::move(receiver), std::move(member), range,
                std::move(arguments));
        };

    for (const auto& structure : _model._structures) {
        const JsonStructureMetadata* metadata =
            _model.JsonMetadataFor(*structure->syntax);
        if (!metadata || !metadata->writer) continue;
        const source::SourceRange range = structure->syntax->Range();
        const auto writeJsonMethod = structure->methods.find("writeJson");
        if (writeJsonMethod == structure->methods.end() ||
            writeJsonMethod->second.empty()) {
            std::vector<std::unique_ptr<core::Statement>> statements;
            bool declaredError = false;
            const auto appendChecked =
                [&](std::vector<std::unique_ptr<core::Statement>>& target,
                    std::unique_ptr<core::Expression> expression) {
                    if (!declaredError) {
                        target.push_back(std::make_unique<
                                         core::VariableDeclarationStatement>(
                            range, nullptr, "err", std::move(expression),
                            range));
                        declaredError = true;
                    } else {
                        auto assignment =
                            std::make_unique<core::AssignmentExpression>(
                                range, named(range, "err"),
                                std::move(expression));
                        target.push_back(
                            std::make_unique<core::ExpressionStatement>(
                                range, std::move(assignment)));
                    }

                    auto condition = std::make_unique<core::BinaryExpression>(
                        range, named(range, "err"), TokenType::NotEqual,
                        std::make_unique<core::LiteralExpression>(
                            range, TokenType::None, "none"));
                    std::vector<std::unique_ptr<core::Statement>> failure;
                    failure.push_back(std::make_unique<core::ReturnStatement>(
                        range, named(range, "err")));
                    auto branch = std::make_unique<core::ConditionalBranch>(
                        range, std::move(condition),
                        std::make_unique<core::BlockStatement>(
                            range, std::move(failure)),
                        nullptr);
                    target.push_back(std::make_unique<core::IfStatement>(
                        range, std::move(branch),
                        std::vector<std::unique_ptr<core::ConditionalBranch>>{},
                        nullptr));
                };
            const auto writerCall =
                [&](std::string method,
                    std::vector<std::unique_ptr<core::Expression>> arguments =
                        {}) {
                    return std::make_unique<core::MemberCallExpression>(
                        range, named(range, "writer"), std::move(method), range,
                        std::move(arguments), TokenType::Arrow);
                };

            std::function<bool(SemanticType)> writerSupports =
                [&](SemanticType type) {
                    if (type.IsNumeric() ||
                        type.Kind() == SemanticTypeKind::Bool ||
                        type.Kind() == SemanticTypeKind::String ||
                        type.Kind() == SemanticTypeKind::StringView)
                        return true;
                    if (type.Kind() == SemanticTypeKind::OwnedSlice)
                        return writerSupports(*type.Target());
                    if (type.Kind() != SemanticTypeKind::Structure)
                        return false;
                    const JsonStructureMetadata* nested =
                        _model.JsonMetadataFor(*type.Structure()->syntax);
                    return nested && nested->writer == metadata->writer;
                };
            using ExpressionFactory =
                std::function<std::unique_ptr<core::Expression>()>;
            std::size_t sequenceIndex = 0;
            std::function<void(std::vector<std::unique_ptr<core::Statement>>&,
                               SemanticType, const ExpressionFactory&)>
                appendValue;
            appendValue = [&](std::vector<std::unique_ptr<core::Statement>>&
                                  target,
                              SemanticType type,
                              const ExpressionFactory& value) {
                if (type.Kind() == SemanticTypeKind::OwnedSlice) {
                    appendChecked(target, writerCall("beginArray"));
                    const std::string indexName =
                        "jsonIndex" + std::to_string(sequenceIndex++);
                    target.push_back(
                        std::make_unique<core::VariableDeclarationStatement>(
                            range,
                            std::make_unique<core::TypeNode>(
                                range, core::PrimitiveType::USize),
                            indexName,
                            std::make_unique<core::LiteralExpression>(
                                range, TokenType::NumberLiteral, "0"),
                            range));
                    auto length = memberCall(range, value(), "len");
                    auto condition = std::make_unique<core::BinaryExpression>(
                        range, named(range, indexName), TokenType::Less,
                        std::move(length));
                    std::vector<std::unique_ptr<core::Statement>> body;
                    appendValue(body, *type.Target(), [&]() {
                        return std::make_unique<core::IndexExpression>(
                            range, value(), named(range, indexName));
                    });
                    auto update = std::make_unique<core::UnaryExpression>(
                        range, TokenType::Increment, named(range, indexName));
                    target.push_back(std::make_unique<core::WhileStatement>(
                        range, std::move(condition),
                        std::make_unique<core::BlockStatement>(range,
                                                               std::move(body)),
                        nullptr, std::move(update)));
                    appendChecked(target, writerCall("endArray"));
                    return;
                }
                if (type.Kind() == SemanticTypeKind::Structure) {
                    std::vector<std::unique_ptr<core::Expression>> arguments;
                    arguments.push_back(named(range, "writer"));
                    appendChecked(target,
                                  memberCall(range, value(), "writeJson",
                                             std::move(arguments)));
                    return;
                }
                std::vector<std::unique_ptr<core::Expression>> arguments;
                arguments.push_back(value());
                appendChecked(target,
                              writerCall("write", std::move(arguments)));
            };

            appendChecked(statements, writerCall("beginObject"));
            for (const JsonFieldMetadata& jsonField : metadata->fields) {
                const StructureField* field = nullptr;
                for (const StructureField& candidate : structure->fields)
                    if (candidate.syntax == jsonField.field) {
                        field = &candidate;
                        break;
                    }
                if (!field) continue;

                if (!writerSupports(field->type)) {
                    Report(diagnostics::DiagnosticCode::InvalidDecoration,
                           field->syntax->NameRange(),
                           "field '" + field->name + "' of type '" +
                               field->type.Name() +
                               "' is not JSON serializable");
                    continue;
                }

                std::vector<std::unique_ptr<core::Expression>> keyArguments;
                keyArguments.push_back(
                    std::make_unique<core::LiteralExpression>(
                        range, TokenType::StringLiteral, jsonField.name));
                appendChecked(statements,
                              writerCall("key", std::move(keyArguments)));

                const ExpressionFactory fieldValue = [&]() {
                    return std::make_unique<core::MemberAccessExpression>(
                        range, named(range, "self"), field->name,
                        field->syntax->NameRange(), TokenType::Arrow);
                };
                appendValue(statements, field->type, fieldValue);
            }
            statements.push_back(std::make_unique<core::ReturnStatement>(
                range, writerCall("endObject")));

            std::vector<std::unique_ptr<core::Parameter>> parameters;
            auto selfType =
                std::make_unique<core::TypeNode>(range, structure->name, true);
            parameters.push_back(std::make_unique<core::Parameter>(
                range,
                std::make_unique<core::TypeNode>(
                    range, core::TypeKind::Reference, std::move(selfType)),
                "self", range));
            auto writerType = std::make_unique<core::TypeNode>(
                range, metadata->writerTypeName);
            parameters.push_back(std::make_unique<core::Parameter>(
                range,
                std::make_unique<core::TypeNode>(
                    range, core::TypeKind::Reference, std::move(writerType)),
                "writer", range));
            auto writeJson = std::make_unique<core::FunctionDeclaration>(
                range, std::vector<core::FunctionModifier>{},
                std::make_unique<core::TypeNode>(
                    range, core::PrimitiveType::ErrorValue),
                "writeJson", std::move(parameters),
                std::make_unique<core::BlockStatement>(range,
                                                       std::move(statements)),
                range, structure->name);
            writeJson->moduleId = structure->syntax->moduleId;
            writeJson->sourceId = structure->syntax->sourceId;
            RegisterFunction(*writeJson, true);
            _model._generatedFunctions.push_back(std::move(writeJson));
        }

        const auto jsonMethod = structure->methods.find("json");
        if (jsonMethod != structure->methods.end() &&
            !jsonMethod->second.empty())
            continue;

        std::vector<std::unique_ptr<core::Statement>> statements;
        auto writerInitializer = std::make_unique<core::MemberCallExpression>(
            range, named(range, metadata->writerTypeName), "init", range,
            std::vector<std::unique_ptr<core::Expression>>{}, TokenType::Scope);
        statements.push_back(
            std::make_unique<core::VariableDeclarationStatement>(
                range, nullptr, "writer", std::move(writerInitializer), range));

        std::vector<std::unique_ptr<core::Expression>> writeArguments;
        writeArguments.push_back(std::make_unique<core::UnaryExpression>(
            range, TokenType::Ampersand, named(range, "writer")));
        auto writeCall = std::make_unique<core::MemberCallExpression>(
            range, named(range, "self"), "writeJson", range,
            std::move(writeArguments), TokenType::Arrow);
        statements.push_back(
            std::make_unique<core::VariableDeclarationStatement>(
                range, nullptr, "err", std::move(writeCall), range));

        auto condition = std::make_unique<core::BinaryExpression>(
            range, named(range, "err"), TokenType::NotEqual,
            std::make_unique<core::LiteralExpression>(range, TokenType::None,
                                                      "none"));
        std::vector<std::unique_ptr<core::Expression>> failureValues;
        failureValues.push_back(std::make_unique<core::LiteralExpression>(
            range, TokenType::None, "none"));
        failureValues.push_back(named(range, "err"));
        std::vector<std::unique_ptr<core::Statement>> failure;
        failure.push_back(std::make_unique<core::ReturnStatement>(
            range, std::make_unique<core::TupleExpression>(
                       range, std::move(failureValues))));
        auto branch = std::make_unique<core::ConditionalBranch>(
            range, std::move(condition),
            std::make_unique<core::BlockStatement>(range, std::move(failure)),
            nullptr);
        statements.push_back(std::make_unique<core::IfStatement>(
            range, std::move(branch),
            std::vector<std::unique_ptr<core::ConditionalBranch>>{}, nullptr));

        auto finishCall = std::make_unique<core::MemberCallExpression>(
            range, named(range, "writer"), "finish", range,
            std::vector<std::unique_ptr<core::Expression>>{});
        statements.push_back(std::make_unique<core::ReturnStatement>(
            range, std::move(finishCall)));

        auto byte =
            std::make_unique<core::TypeNode>(range, core::PrimitiveType::U8);
        auto bytes = std::make_unique<core::TypeNode>(
            range, core::TypeKind::Slice, std::move(byte));
        bytes->SetContract(core::OwnershipContract::Own);
        std::vector<std::unique_ptr<core::TypeNode>> results;
        results.push_back(std::move(bytes));
        results.push_back(std::make_unique<core::TypeNode>(
            range, core::PrimitiveType::ErrorValue));

        std::vector<std::unique_ptr<core::Parameter>> parameters;
        auto selfType =
            std::make_unique<core::TypeNode>(range, structure->name, true);
        parameters.push_back(std::make_unique<core::Parameter>(
            range,
            std::make_unique<core::TypeNode>(range, core::TypeKind::Reference,
                                             std::move(selfType)),
            "self", range));
        auto json = std::make_unique<core::FunctionDeclaration>(
            range, std::vector<core::FunctionModifier>{},
            std::make_unique<core::TypeNode>(range, std::move(results)), "json",
            std::move(parameters),
            std::make_unique<core::BlockStatement>(range,
                                                   std::move(statements)),
            range, structure->name);
        json->moduleId = structure->syntax->moduleId;
        json->sourceId = structure->syntax->sourceId;
        RegisterFunction(*json, true);
        _model._generatedFunctions.push_back(std::move(json));
    }

    const auto none = [](source::SourceRange range) {
        return std::make_unique<core::LiteralExpression>(range, TokenType::None,
                                                         "none");
    };
    const auto assignmentStatement =
        [](source::SourceRange range,
           std::unique_ptr<core::Expression> destination,
           std::unique_ptr<core::Expression> value) {
            return std::make_unique<core::ExpressionStatement>(
                range, std::make_unique<core::AssignmentExpression>(
                           range, std::move(destination), std::move(value)));
        };
    const auto errorReturn = [&](source::SourceRange range,
                                 std::string errorName) {
        auto condition = std::make_unique<core::BinaryExpression>(
            range, named(range, errorName), TokenType::NotEqual, none(range));
        std::vector<std::unique_ptr<core::Statement>> failure;
        failure.push_back(std::make_unique<core::ReturnStatement>(
            range, named(range, errorName)));
        auto branch = std::make_unique<core::ConditionalBranch>(
            range, std::move(condition),
            std::make_unique<core::BlockStatement>(range, std::move(failure)),
            nullptr);
        return std::make_unique<core::IfStatement>(
            range, std::move(branch),
            std::vector<std::unique_ptr<core::ConditionalBranch>>{}, nullptr);
    };
    const auto tupleReturn = [&](source::SourceRange range,
                                 std::string valueName,
                                 std::unique_ptr<core::Expression> error) {
        std::vector<std::unique_ptr<core::Expression>> values;
        values.push_back(named(range, std::move(valueName)));
        values.push_back(std::move(error));
        return std::make_unique<core::ReturnStatement>(
            range,
            std::make_unique<core::TupleExpression>(range, std::move(values)));
    };

    for (const auto& structure : _model._structures) {
        const JsonStructureMetadata* metadata =
            _model.JsonMetadataFor(*structure->syntax);
        if (!metadata || !metadata->reader) continue;
        const source::SourceRange range = structure->syntax->Range();
        const auto readerCall =
            [&](std::string method,
                std::vector<std::unique_ptr<core::Expression>> arguments = {}) {
                return std::make_unique<core::MemberCallExpression>(
                    range, named(range, "reader"), std::move(method), range,
                    std::move(arguments), TokenType::Arrow);
            };
        const auto selfField = [&](const StructureField& field) {
            return std::make_unique<core::MemberAccessExpression>(
                range, named(range, "self"), field.name,
                field.syntax->NameRange(), TokenType::Arrow);
        };

        const auto readJsonMethod = structure->methods.find("readJson");
        if (readJsonMethod == structure->methods.end() ||
            readJsonMethod->second.empty()) {
            std::vector<std::unique_ptr<core::Statement>> statements;
            statements.push_back(
                std::make_unique<core::VariableDeclarationStatement>(
                    range, nullptr, "err", readerCall("beginObject"), range));
            statements.push_back(errorReturn(range, "err"));

            std::vector<std::unique_ptr<core::DestructureBinding>> firstNext;
            firstNext.push_back(std::make_unique<core::DestructureBinding>(
                range, "more", nullptr));
            firstNext.push_back(std::make_unique<core::DestructureBinding>(
                range, "err", nullptr));
            statements.push_back(std::make_unique<core::DestructureStatement>(
                range, std::move(firstNext), readerCall("nextObjectField"),
                true));

            std::function<bool(SemanticType)> readerSupports =
                [&](SemanticType type) {
                    if (type.IsNumeric() ||
                        type.Kind() == SemanticTypeKind::Bool ||
                        type.Kind() == SemanticTypeKind::String)
                        return true;
                    if (type.Kind() == SemanticTypeKind::OwnedSlice)
                        return readerSupports(*type.Target());
                    if (type.Kind() != SemanticTypeKind::Structure)
                        return false;
                    const JsonStructureMetadata* nested =
                        _model.JsonMetadataFor(*type.Structure()->syntax);
                    return nested && nested->reader == metadata->reader;
                };
            const auto containsBorrowedJsonDestination =
                [&](const auto& self, SemanticType type) -> bool {
                if (type.Kind() == SemanticTypeKind::Slice ||
                    type.Kind() == SemanticTypeKind::StringView)
                    return true;
                return type.Kind() == SemanticTypeKind::OwnedSlice &&
                       self(self, *type.Target());
            };
            using ExpressionFactory =
                std::function<std::unique_ptr<core::Expression>()>;
            std::size_t sequenceIndex = 0;
            const auto appendWhenNoError =
                [&](std::vector<std::unique_ptr<core::Statement>>& target,
                    std::vector<std::unique_ptr<core::Statement>> success) {
                    auto condition = std::make_unique<core::BinaryExpression>(
                        range, named(range, "err"), TokenType::Equal,
                        none(range));
                    auto branch = std::make_unique<core::ConditionalBranch>(
                        range, std::move(condition),
                        std::make_unique<core::BlockStatement>(
                            range, std::move(success)),
                        nullptr);
                    target.push_back(std::make_unique<core::IfStatement>(
                        range, std::move(branch),
                        std::vector<std::unique_ptr<core::ConditionalBranch>>{},
                        nullptr));
                };
            std::function<void(std::vector<std::unique_ptr<core::Statement>>&,
                               SemanticType, const ExpressionFactory&)>
                appendReadValue;
            appendReadValue = [&](std::vector<std::unique_ptr<core::Statement>>&
                                      target,
                                  SemanticType type,
                                  const ExpressionFactory& destination) {
                if (type.Kind() == SemanticTypeKind::OwnedSlice) {
                    target.push_back(assignmentStatement(
                        range, named(range, "err"), readerCall("beginArray")));
                    std::vector<std::unique_ptr<core::Statement>> arrayBody;
                    const std::size_t id = sequenceIndex++;
                    const std::string valuesName =
                        "jsonValues" + std::to_string(id);
                    const std::string moreName =
                        "jsonMore" + std::to_string(id);
                    const std::string elementName =
                        "jsonElement" + std::to_string(id);
                    const std::string grownName =
                        "jsonGrown" + std::to_string(id);
                    const std::string appendErrorName =
                        "jsonAppendError" + std::to_string(id);

                    arrayBody.push_back(
                        std::make_unique<core::VariableDeclarationStatement>(
                            range, TypeSyntax(type, range), valuesName,
                            none(range), range));

                    std::vector<std::unique_ptr<core::Statement>> decodeBody;
                    decodeBody.push_back(
                        std::make_unique<core::VariableDeclarationStatement>(
                            range,
                            std::make_unique<core::TypeNode>(
                                range, core::PrimitiveType::Bool),
                            moreName, none(range), range));
                    std::vector<std::unique_ptr<core::DestructureBinding>>
                        firstElement;
                    firstElement.push_back(
                        std::make_unique<core::DestructureBinding>(
                            range, moreName, nullptr));
                    firstElement.push_back(
                        std::make_unique<core::DestructureBinding>(range, "err",
                                                                   nullptr));
                    decodeBody.push_back(
                        std::make_unique<core::DestructureStatement>(
                            range, std::move(firstElement),
                            readerCall("nextArrayElement"), false));

                    std::vector<std::unique_ptr<core::Statement>> loopBody;
                    loopBody.push_back(
                        std::make_unique<core::VariableDeclarationStatement>(
                            range, TypeSyntax(*type.Target(), range),
                            elementName, none(range), range));
                    appendReadValue(loopBody, *type.Target(), [&]() {
                        return named(range, elementName);
                    });

                    std::vector<std::unique_ptr<core::Statement>> appendBody;
                    std::vector<std::unique_ptr<core::Expression>> arguments;
                    arguments.push_back(named(range, valuesName));
                    arguments.push_back(named(range, elementName));
                    auto append = std::make_unique<core::CallExpression>(
                        range, named(range, "__json_slice_append"),
                        std::move(arguments),
                        TypeSyntax(*type.Target(), range));
                    std::vector<std::unique_ptr<core::DestructureBinding>>
                        appended;
                    appended.push_back(
                        std::make_unique<core::DestructureBinding>(
                            range, grownName, nullptr));
                    appended.push_back(
                        std::make_unique<core::DestructureBinding>(
                            range, appendErrorName, nullptr));
                    appendBody.push_back(
                        std::make_unique<core::DestructureStatement>(
                            range, std::move(appended), std::move(append),
                            true));
                    appendBody.push_back(
                        assignmentStatement(range, named(range, valuesName),
                                            named(range, grownName)));
                    appendBody.push_back(
                        assignmentStatement(range, named(range, "err"),
                                            named(range, appendErrorName)));
                    appendWhenNoError(loopBody, std::move(appendBody));

                    std::vector<std::unique_ptr<core::Statement>> advance;
                    std::vector<std::unique_ptr<core::DestructureBinding>>
                        nextElement;
                    nextElement.push_back(
                        std::make_unique<core::DestructureBinding>(
                            range, moreName, nullptr));
                    nextElement.push_back(
                        std::make_unique<core::DestructureBinding>(range, "err",
                                                                   nullptr));
                    advance.push_back(
                        std::make_unique<core::DestructureStatement>(
                            range, std::move(nextElement),
                            readerCall("nextArrayElement"), false));
                    appendWhenNoError(loopBody, std::move(advance));

                    auto noError = std::make_unique<core::BinaryExpression>(
                        range, named(range, "err"), TokenType::Equal,
                        none(range));
                    auto loopCondition =
                        std::make_unique<core::BinaryExpression>(
                            range, std::move(noError), TokenType::And,
                            named(range, moreName));
                    decodeBody.push_back(std::make_unique<core::WhileStatement>(
                        range, std::move(loopCondition),
                        std::make_unique<core::BlockStatement>(
                            range, std::move(loopBody)),
                        nullptr));

                    std::vector<std::unique_ptr<core::Statement>> assign;
                    assign.push_back(assignmentStatement(
                        range, destination(), named(range, valuesName)));
                    appendWhenNoError(decodeBody, std::move(assign));
                    appendWhenNoError(arrayBody, std::move(decodeBody));
                    appendWhenNoError(target, std::move(arrayBody));
                    return;
                }

                if (type.IsNumeric() || type.Kind() == SemanticTypeKind::Bool) {
                    std::vector<std::unique_ptr<core::Expression>> arguments;
                    arguments.push_back(std::make_unique<core::UnaryExpression>(
                        range, TokenType::Ampersand, destination()));
                    target.push_back(assignmentStatement(
                        range, named(range, "err"),
                        readerCall("read", std::move(arguments))));
                    return;
                }
                if (type.Kind() == SemanticTypeKind::String) {
                    const std::string decodedName =
                        "jsonDecoded" + std::to_string(sequenceIndex++);
                    target.push_back(
                        std::make_unique<core::VariableDeclarationStatement>(
                            range, TypeSyntax(type, range), decodedName,
                            none(range), range));
                    std::vector<std::unique_ptr<core::DestructureBinding>>
                        decoded;
                    decoded.push_back(
                        std::make_unique<core::DestructureBinding>(
                            range, decodedName, nullptr));
                    decoded.push_back(
                        std::make_unique<core::DestructureBinding>(range, "err",
                                                                   nullptr));
                    target.push_back(
                        std::make_unique<core::DestructureStatement>(
                            range, std::move(decoded), readerCall("readString"),
                            false));
                    std::vector<std::unique_ptr<core::Statement>> assign;
                    assign.push_back(assignmentStatement(
                        range, destination(), named(range, decodedName)));
                    appendWhenNoError(target, std::move(assign));
                    return;
                }

                std::vector<std::unique_ptr<core::Expression>> arguments;
                arguments.push_back(named(range, "reader"));
                target.push_back(assignmentStatement(
                    range, named(range, "err"),
                    memberCall(range, destination(), "readJson",
                               std::move(arguments))));
            };

            std::vector<std::unique_ptr<core::ConditionalBranch>> branches;
            for (const JsonFieldMetadata& jsonField : metadata->fields) {
                const StructureField* field = nullptr;
                for (const StructureField& candidate : structure->fields)
                    if (candidate.syntax == jsonField.field) {
                        field = &candidate;
                        break;
                    }
                if (!field) continue;

                if (!readerSupports(field->type)) {
                    Report(
                        diagnostics::DiagnosticCode::InvalidDecoration,
                        field->syntax->NameRange(),
                        "field '" + field->name + "' of type '" +
                            field->type.Name() +
                            "' cannot be JSON deserialized" +
                            (containsBorrowedJsonDestination(
                                 containsBorrowedJsonDestination, field->type)
                                 ? " because it does not own its decoded "
                                   "value"
                                 : ""));
                    continue;
                }

                std::vector<std::unique_ptr<core::Expression>> keyArguments;
                keyArguments.push_back(
                    std::make_unique<core::LiteralExpression>(
                        range, TokenType::StringLiteral, jsonField.name));
                auto keyCondition =
                    readerCall("keyIs", std::move(keyArguments));
                std::vector<std::unique_ptr<core::Statement>> body;
                appendReadValue(body, field->type,
                                [&]() { return selfField(*field); });
                branches.push_back(std::make_unique<core::ConditionalBranch>(
                    range, std::move(keyCondition),
                    std::make_unique<core::BlockStatement>(range,
                                                           std::move(body)),
                    nullptr));
            }

            std::vector<std::unique_ptr<core::Statement>> unknown;
            unknown.push_back(assignmentStatement(range, named(range, "err"),
                                                  readerCall("skipValue")));
            std::unique_ptr<core::IfStatement> dispatch;
            if (branches.empty()) {
                dispatch = nullptr;
            } else {
                auto primary = std::move(branches.front());
                branches.erase(branches.begin());
                dispatch = std::make_unique<core::IfStatement>(
                    range, std::move(primary), std::move(branches),
                    std::make_unique<core::BlockStatement>(range,
                                                           std::move(unknown)));
            }

            std::vector<std::unique_ptr<core::Statement>> loopBody;
            if (dispatch) {
                loopBody.push_back(std::move(dispatch));
            } else {
                loopBody.push_back(assignmentStatement(
                    range, named(range, "err"), readerCall("skipValue")));
            }

            auto successfulField = std::make_unique<core::BinaryExpression>(
                range, named(range, "err"), TokenType::Equal, none(range));
            std::vector<std::unique_ptr<core::DestructureBinding>> next;
            next.push_back(std::make_unique<core::DestructureBinding>(
                range, "more", nullptr));
            next.push_back(std::make_unique<core::DestructureBinding>(
                range, "err", nullptr));
            std::vector<std::unique_ptr<core::Statement>> advance;
            advance.push_back(std::make_unique<core::DestructureStatement>(
                range, std::move(next), readerCall("nextObjectField"), false));
            auto advanceBranch = std::make_unique<core::ConditionalBranch>(
                range, std::move(successfulField),
                std::make_unique<core::BlockStatement>(range,
                                                       std::move(advance)),
                nullptr);
            loopBody.push_back(std::make_unique<core::IfStatement>(
                range, std::move(advanceBranch),
                std::vector<std::unique_ptr<core::ConditionalBranch>>{},
                nullptr));

            auto noError = std::make_unique<core::BinaryExpression>(
                range, named(range, "err"), TokenType::Equal, none(range));
            auto condition = std::make_unique<core::BinaryExpression>(
                range, std::move(noError), TokenType::And,
                named(range, "more"));
            statements.push_back(std::make_unique<core::WhileStatement>(
                range, std::move(condition),
                std::make_unique<core::BlockStatement>(range,
                                                       std::move(loopBody)),
                nullptr));
            statements.push_back(std::make_unique<core::ReturnStatement>(
                range, named(range, "err")));

            std::vector<std::unique_ptr<core::Parameter>> parameters;
            parameters.push_back(std::make_unique<core::Parameter>(
                range,
                std::make_unique<core::TypeNode>(
                    range, core::TypeKind::Reference,
                    std::make_unique<core::TypeNode>(range, structure->name)),
                "self", range));
            parameters.push_back(std::make_unique<core::Parameter>(
                range,
                std::make_unique<core::TypeNode>(
                    range, core::TypeKind::Reference,
                    std::make_unique<core::TypeNode>(range,
                                                     metadata->readerTypeName)),
                "reader", range));
            auto readJson = std::make_unique<core::FunctionDeclaration>(
                range, std::vector<core::FunctionModifier>{},
                std::make_unique<core::TypeNode>(
                    range, core::PrimitiveType::ErrorValue),
                "readJson", std::move(parameters),
                std::make_unique<core::BlockStatement>(range,
                                                       std::move(statements)),
                range, structure->name);
            readJson->moduleId = structure->syntax->moduleId;
            readJson->sourceId = structure->syntax->sourceId;
            RegisterFunction(*readJson, true);
            _model._generatedFunctions.push_back(std::move(readJson));
        }

        const auto fromJsonMethod = structure->methods.find("fromJson");
        if (fromJsonMethod != structure->methods.end() &&
            !fromJsonMethod->second.empty())
            continue;

        std::vector<std::unique_ptr<core::Statement>> statements;
        std::vector<std::unique_ptr<core::Expression>> sourceArgument;
        sourceArgument.push_back(named(range, "source"));
        statements.push_back(
            std::make_unique<core::VariableDeclarationStatement>(
                range, nullptr, "reader",
                std::make_unique<core::MemberCallExpression>(
                    range, named(range, metadata->readerTypeName), "init",
                    range, std::move(sourceArgument), TokenType::Scope),
                range));
        statements.push_back(
            std::make_unique<core::VariableDeclarationStatement>(
                range, nullptr, "value",
                std::make_unique<core::MemberCallExpression>(
                    range, named(range, structure->name), "zero", range,
                    std::vector<std::unique_ptr<core::Expression>>{},
                    TokenType::Scope),
                range));

        std::vector<std::unique_ptr<core::Expression>> readArguments;
        readArguments.push_back(std::make_unique<core::UnaryExpression>(
            range, TokenType::Ampersand, named(range, "reader")));
        statements.push_back(
            std::make_unique<core::VariableDeclarationStatement>(
                range, nullptr, "err",
                memberCall(range, named(range, "value"), "readJson",
                           std::move(readArguments)),
                range));

        const auto appendFailure = [&]() {
            auto condition = std::make_unique<core::BinaryExpression>(
                range, named(range, "err"), TokenType::NotEqual, none(range));
            std::vector<std::unique_ptr<core::Statement>> failure;
            failure.push_back(tupleReturn(range, "value", named(range, "err")));
            auto branch = std::make_unique<core::ConditionalBranch>(
                range, std::move(condition),
                std::make_unique<core::BlockStatement>(range,
                                                       std::move(failure)),
                nullptr);
            statements.push_back(std::make_unique<core::IfStatement>(
                range, std::move(branch),
                std::vector<std::unique_ptr<core::ConditionalBranch>>{},
                nullptr));
        };
        appendFailure();
        statements.push_back(assignmentStatement(
            range, named(range, "err"),
            memberCall(range, named(range, "reader"), "finish")));
        appendFailure();
        statements.push_back(tupleReturn(range, "value", none(range)));

        auto byte = std::make_unique<core::TypeNode>(
            range, core::PrimitiveType::U8, true);
        auto sourceType = std::make_unique<core::TypeNode>(
            range, core::TypeKind::Slice, std::move(byte));
        std::vector<std::unique_ptr<core::Parameter>> parameters;
        parameters.push_back(std::make_unique<core::Parameter>(
            range, std::move(sourceType), "source", range));
        std::vector<std::unique_ptr<core::TypeNode>> results;
        results.push_back(
            std::make_unique<core::TypeNode>(range, structure->name));
        results.push_back(std::make_unique<core::TypeNode>(
            range, core::PrimitiveType::ErrorValue));
        auto fromJson = std::make_unique<core::FunctionDeclaration>(
            range, std::vector<core::FunctionModifier>{},
            std::make_unique<core::TypeNode>(range, std::move(results)),
            "fromJson", std::move(parameters),
            std::make_unique<core::BlockStatement>(range,
                                                   std::move(statements)),
            range, structure->name);
        fromJson->moduleId = structure->syntax->moduleId;
        fromJson->sourceId = structure->syntax->sourceId;
        RegisterFunction(*fromJson, true);
        _model._generatedFunctions.push_back(std::move(fromJson));
    }
}

void SemanticAnalyzer::CheckMethodAccess(const FunctionSymbol& function,
                                         source::SourceRange range) {
    if (function.Name().ends_with("::tear"))
        Report(diagnostics::DiagnosticCode::InvalidLifecycle, range,
               "tear is compiler-only and cannot be called directly");
    if (function.IsPrivate() &&
        ((function.Owner() && CurrentOwner() != function.Owner()) ||
         (function.EnumOwner() && CurrentEnumOwner() != function.EnumOwner())))
        Report(diagnostics::DiagnosticCode::PrivateAccess, range,
               "method '" + function.Name() + "' is private");
    if (function.IsProtected()) {
        const InterfaceType* declaringInterface = function.InterfaceOwner();
        const MethodKey key = MethodKeyOf(function);
        const auto findDeclaration = [&](const auto& conformances) {
            for (const auto& conformance : conformances)
                for (const MethodContract* contract : conformance.type->methods)
                    if (contract->key == key &&
                        contract->visibility == MethodVisibility::Protected)
                        return contract->declaredBy;
            return static_cast<const InterfaceType*>(nullptr);
        };
        if (declaringInterface == nullptr && function.Owner())
            declaringInterface =
                findDeclaration(function.Owner()->conformances);
        if (declaringInterface == nullptr && function.EnumOwner())
            declaringInterface =
                findDeclaration(function.EnumOwner()->conformances);

        bool accessible = false;
        if (const InterfaceType* caller = CurrentInterfaceOwner())
            accessible =
                caller == declaringInterface ||
                std::ranges::find(caller->ancestors, declaringInterface) !=
                    caller->ancestors.end();
        if (CurrentOwner())
            accessible = std::ranges::find(CurrentOwner()->conformanceClosure,
                                           declaringInterface) !=
                         CurrentOwner()->conformanceClosure.end();
        if (CurrentEnumOwner())
            accessible =
                std::ranges::find(CurrentEnumOwner()->conformanceClosure,
                                  declaringInterface) !=
                CurrentEnumOwner()->conformanceClosure.end();
        if (!accessible) {
            const std::string name = function.Declaration()
                                         ? function.Declaration()->Name()
                                         : function.Name();
            Report(diagnostics::DiagnosticCode::PrivateAccess, range,
                   "method '" + name + "' is protected");
        }
    }
    if (function.IsUnsafe() ||
        (function.ReturnType().IsImmovable() && !_inZero))
        RequireUnsafe(range);
}

void SemanticAnalyzer::CheckTransfer(const core::Expression& expression,
                                     SemanticType type,
                                     bool consumingOwnership) {
    const core::Expression* source = &expression;
    while (source->Kind() == core::AstNodeKind::ParenthesizedExpression)
        source =
            &static_cast<const core::ParenthesizedExpression&>(*source).Inner();
    const bool semanticDefault = IsNoneLiteral(*source);
    const SemanticType sourceType = _model.TypeOf(expression);
    if (sourceType.ContainsOwnedBuffer()) type = sourceType;
    const Symbol* sourceSymbol =
        source->Kind() == core::AstNodeKind::NameExpression
            ? _model.SymbolFor(
                  static_cast<const core::NameExpression&>(*source))
            : nullptr;
    const auto* sourceCapture =
        sourceSymbol != nullptr ? _model.CaptureFor(*sourceSymbol) : nullptr;
    const bool ownedCapture =
        sourceCapture != nullptr &&
        sourceCapture->Mode() ==
            core::AnonymousFunctionExpression::CaptureMode::Own;
    if (consumingOwnership && ownedCapture) {
        if (_currentFunction == nullptr || !_currentFunction->IsOnce()) {
            Report(diagnostics::DiagnosticCode::NonCopyableValue,
                   expression.Range(),
                   "owned capture '" + sourceSymbol->Name() +
                       "' cannot be consumed by a reusable closure");
            return;
        }
        if (type.IsImmovable()) {
            Report(diagnostics::DiagnosticCode::NonCopyableValue,
                   expression.Range(),
                   "'" + type.Name() + "' is immovable; borrow it instead");
            return;
        }
        _model._transfers.insert_or_assign(&expression, TransferKind::Move);
        return;
    }
    if (_currentFunction && CopyMayAllocate(type) &&
        source->Kind() != core::AstNodeKind::BuildExpression &&
        source->Kind() != core::AstNodeKind::CallExpression &&
        source->Kind() != core::AstNodeKind::MemberCallExpression &&
        !semanticDefault)
        _forbiddenZeroEffects.insert(_currentFunction);
    if ((!type.OwnsStorage() && !type.ContainsOwnedBuffer()) ||
        type.IsCopyable()) {
        _model._transfers.insert_or_assign(&expression, TransferKind::Copy);
        return;
    }
    if (semanticDefault) {
        _model._transfers.insert_or_assign(&expression, TransferKind::Direct);
        return;
    }
    if (expression.Kind() == core::AstNodeKind::ParenthesizedExpression) {
        CheckTransfer(
            static_cast<const core::ParenthesizedExpression&>(expression)
                .Inner(),
            type, consumingOwnership);
        return;
    }
    if (expression.Kind() == core::AstNodeKind::BuildExpression ||
        expression.Kind() == core::AstNodeKind::CallExpression ||
        expression.Kind() == core::AstNodeKind::AnonymousFunctionExpression) {
        _model._transfers.insert_or_assign(&expression, TransferKind::Direct);
        return;
    }
    if (expression.Kind() == core::AstNodeKind::MemberCallExpression &&
        _model.MethodFor(
            static_cast<const core::MemberCallExpression&>(expression))) {
        _model._transfers.insert_or_assign(&expression, TransferKind::Direct);
        return;
    }
    if (expression.Kind() == core::AstNodeKind::NameExpression) {
        const auto& name = static_cast<const core::NameExpression&>(expression);
        const Symbol* symbol = _model.SymbolFor(name);
        if (type.IsImmovable()) {
            Report(diagnostics::DiagnosticCode::NonCopyableValue,
                   expression.Range(),
                   "'" + type.Name() + "' is immovable; borrow it instead");
            return;
        }
        if (symbol != nullptr && symbol->Kind() == SymbolKind::Function) {
            _model._transfers.insert_or_assign(&expression,
                                               TransferKind::Direct);
            return;
        }
        if (symbol != nullptr) {
            const auto* capture = _model.CaptureFor(*symbol);
            if (capture != nullptr &&
                capture->Mode() ==
                    core::AnonymousFunctionExpression::CaptureMode::Own &&
                (_currentFunction == nullptr || !_currentFunction->IsOnce())) {
                Report(diagnostics::DiagnosticCode::NonCopyableValue,
                       expression.Range(),
                       "owned capture '" + symbol->Name() +
                           "' cannot be consumed by a reusable closure");
                return;
            }
            _model._transfers.insert_or_assign(&expression, TransferKind::Move);
            return;
        }
    }
    if (expression.Kind() == core::AstNodeKind::TupleExpression &&
        type.Kind() == SemanticTypeKind::Tuple) {
        const auto& tuple =
            static_cast<const core::TupleExpression&>(expression);
        for (std::size_t i = 0;
             i < tuple.Elements().size() && i < type.Elements().size(); ++i)
            CheckTransfer(*tuple.Elements()[i], type.Elements()[i],
                          consumingOwnership);
        _model._transfers.insert_or_assign(&expression, TransferKind::Direct);
        return;
    }
    if (expression.Kind() == core::AstNodeKind::ArrayExpression &&
        type.Kind() == SemanticTypeKind::Array) {
        const auto& array =
            static_cast<const core::ArrayExpression&>(expression);
        if (!array.IsRepeat() || array.Length() <= 1) {
            for (const auto& element : array.Elements())
                CheckTransfer(*element, *type.Target(), consumingOwnership);
            _model._transfers.insert_or_assign(&expression,
                                               TransferKind::Direct);
            return;
        }
    }
    if (expression.Kind() == core::AstNodeKind::MatchExpression) {
        const auto& match =
            static_cast<const core::MatchExpression&>(expression);
        for (const auto& arm : match.Arms())
            CheckTransfer(arm->Value(), type, consumingOwnership);
        _model._transfers.insert_or_assign(&expression, TransferKind::Direct);
        return;
    }
    Report(diagnostics::DiagnosticCode::NonCopyableValue, expression.Range(),
           "cannot partially move '" + type.Name() +
               "'; move the complete variable instead");
}

SemanticType SemanticAnalyzer::AnalyzeBuild(
    const core::BuildExpression& expression) {
    const SemanticType type = ResolveType(expression.Type());
    if (type.Kind() != SemanticTypeKind::Structure) {
        Report(diagnostics::DiagnosticCode::InvalidStructure,
               expression.Type().Range(), "build requires a structure type");
        return SemanticType::Error();
    }
    if (type.IsImmovable() && !_inZero) RequireUnsafe(expression.Range());
    const bool zeroBuild = _currentFunction && _currentFunction->Owner() &&
                           _currentFunction->Name().ends_with("::zero");
    if (zeroBuild)
        for (const auto& field : type.Structure()->fields)
            RecordZeroDependency(field.type);
    if (!zeroBuild && _currentFunction && type.Structure()->init)
        for (const auto& field : type.Structure()->fields)
            if (field.syntax->Initializer()) {
                _functionCalls[_currentFunction].insert(type.Structure()->init);
                break;
            }
    std::unordered_set<const StructureField*> requiredInitializers;
    for (const auto& field : type.Structure()->fields)
        if (!field.syntax->Initializer() && field.type.ContainsReference())
            requiredInitializers.insert(&field);
    std::unordered_set<const StructureField*> initializedFields;
    bool reportedInitializationOrder = false;
    for (const auto& statement : expression.Body().Statements()) {
        const core::Expression* value = nullptr;
        if (statement->Kind() == core::AstNodeKind::ExpressionStatement)
            value = &static_cast<const core::ExpressionStatement&>(*statement)
                         .Value();
        while (value &&
               value->Kind() == core::AstNodeKind::ParenthesizedExpression)
            value = &static_cast<const core::ParenthesizedExpression&>(*value)
                         .Inner();
        const core::AssignmentExpression* assignment =
            value && value->Kind() == core::AstNodeKind::AssignmentExpression
                ? &static_cast<const core::AssignmentExpression&>(*value)
                : nullptr;
        const core::Expression* target =
            assignment && assignment->OperatorType() == TokenType::Assign
                ? &assignment->Target()
                : nullptr;
        while (target &&
               target->Kind() == core::AstNodeKind::ParenthesizedExpression)
            target = &static_cast<const core::ParenthesizedExpression&>(*target)
                          .Inner();
        const core::MemberAccessExpression* member =
            target &&
                    target->Kind() == core::AstNodeKind::MemberAccessExpression
                ? &static_cast<const core::MemberAccessExpression&>(*target)
                : nullptr;
        const core::Expression* receiver =
            member ? &member->Receiver() : nullptr;
        while (receiver &&
               receiver->Kind() == core::AstNodeKind::ParenthesizedExpression)
            receiver =
                &static_cast<const core::ParenthesizedExpression&>(*receiver)
                     .Inner();
        const bool directSelf =
            member && member->Access() == TokenType::Dot && receiver &&
            receiver->Kind() == core::AstNodeKind::NameExpression &&
            static_cast<const core::NameExpression&>(*receiver).Name() ==
                "self";
        const StructureField* initialized = nullptr;
        if (directSelf)
            for (const auto& field : type.Structure()->fields)
                if (field.name == member->Member()) {
                    initialized = &field;
                    break;
                }
        if (initialized && requiredInitializers.contains(initialized) &&
            !initializedFields.contains(initialized)) {
            initializedFields.insert(initialized);
            _model._buildInitializers.insert(assignment);
            continue;
        }
        if (initializedFields.size() < requiredInitializers.size() &&
            !reportedInitializationOrder) {
            Report(diagnostics::DiagnosticCode::InvalidStructure,
                   statement->Range(),
                   "reference-bearing fields must be initialized before "
                   "other build statements");
            reportedInitializationOrder = true;
        }
    }
    for (const StructureField* field : requiredInitializers)
        if (!initializedFields.contains(field))
            Report(diagnostics::DiagnosticCode::InvalidStructure,
                   expression.Range(),
                   "reference-bearing field '" + field->name +
                       "' requires direct initialization in build");
    bool invalidDefaultConstruction = false;
    std::function<void(SemanticType)> checkDefault = [&](SemanticType field) {
        if (field.Kind() == SemanticTypeKind::Enum && !field.IsZeroable()) {
            invalidDefaultConstruction = true;
            Report(diagnostics::DiagnosticCode::InvalidStructure,
                   expression.Range(),
                   "field of enum type '" + field.Name() +
                       "' has no default variant");
        }
        if (field.Kind() == SemanticTypeKind::Structure) {
            const FunctionSymbol* method =
                zeroBuild ? field.Structure()->zero : field.Structure()->init;
            if (!method && zeroBuild) {
                invalidDefaultConstruction = true;
                Report(diagnostics::DiagnosticCode::InvalidStructure,
                       expression.Range(),
                       "field of type '" + field.Name() +
                           "' has no valid default construction");
            }
            if (method) {
                CheckMethodAccess(*method, expression.Range());
                if (_currentFunction)
                    _functionCalls[_currentFunction].insert(method);
            }
            if (method && !method->ParameterTypes().empty()) {
                invalidDefaultConstruction = true;
                Report(diagnostics::DiagnosticCode::InvalidStructure,
                       expression.Range(),
                       "field of type '" + field.Name() +
                           "' needs an explicit declaration initializer "
                           "because its init requires arguments");
            }
        }
        if (field.Kind() == SemanticTypeKind::Array)
            checkDefault(*field.Target());
        if (field.Kind() == SemanticTypeKind::Tuple)
            for (const auto& element : field.Elements()) checkDefault(element);
    };
    for (const auto& field : type.Structure()->fields)
        if (!initializedFields.contains(&field) &&
            (zeroBuild || !field.syntax->Initializer()))
            checkDefault(field.type);
    if (invalidDefaultConstruction)
        for (auto& structure : _model._structures)
            if (structure.get() == type.Structure()) {
                structure->invalidDefaultConstruction = true;
                break;
            }
    Scope* previousScope = _currentScope;
    const Symbol* previousSelf = _buildSelf;
    _currentScope = &AddScope(previousScope);
    _buildSelf = AddSymbol(
        std::make_unique<VariableSymbol>(SymbolKind::LocalVariable, "self",
                                         type, expression.Range(), true),
        expression);
    const std::size_t previousLoopDepth = _loopDepth;
    _loopDepth = 0;
    ++_buildDepth;
    AnalyzeBlock(expression.Body(), false);
    --_buildDepth;
    _loopDepth = previousLoopDepth;
    _buildSelf = previousSelf;
    _currentScope = previousScope;
    return type;
}

SemanticType SemanticAnalyzer::AnalyzeMethodCall(
    const core::MemberCallExpression& expression, SemanticType receiver) {
    const bool isStatic = expression.Access() == TokenType::Scope;
    const StructureType* owner = nullptr;
    const EnumType* enumOwner = nullptr;
    const InterfaceType* interfaceOwner = nullptr;
    if (isStatic) {
        if (expression.Receiver().Kind() == core::AstNodeKind::NameExpression) {
            const Symbol* type = _currentScope->Lookup(
                static_cast<const core::NameExpression&>(expression.Receiver())
                    .Name());
            if (type && type->Kind() == SymbolKind::StructureType)
                owner = type->Type().Structure();
            if (type && type->Kind() == SymbolKind::EnumType)
                enumOwner = type->Type().Enum();
            if (type && type->Kind() == SymbolKind::InterfaceType)
                interfaceOwner = type->Type().Interface();
        }
    } else {
        if (receiver.Kind() == SemanticTypeKind::Structure ||
            receiver.Kind() == SemanticTypeKind::Enum ||
            receiver.Kind() == SemanticTypeKind::Interface) {
            if (expression.Access() != TokenType::Dot)
                Report(diagnostics::DiagnosticCode::InvalidMember,
                       expression.Range(), "value receivers use '.'");
            if (receiver.Kind() == SemanticTypeKind::Structure)
                owner = receiver.Structure();
            else if (receiver.Kind() == SemanticTypeKind::Enum)
                enumOwner = receiver.Enum();
            else {
                interfaceOwner = receiver.Interface();
                Report(diagnostics::DiagnosticCode::InvalidInterface,
                       expression.Receiver().Range(),
                       "interface methods require a checked-reference "
                       "receiver");
            }
        } else if (receiver.Kind() == SemanticTypeKind::Pointer ||
                   receiver.Kind() == SemanticTypeKind::Reference) {
            if (expression.Access() != TokenType::Arrow)
                Report(diagnostics::DiagnosticCode::InvalidMember,
                       expression.Range(),
                       "pointer and reference receivers use '->'");
            if (receiver.Target()->Kind() == SemanticTypeKind::Structure)
                owner = receiver.Target()->Structure();
            else if (receiver.Target()->Kind() == SemanticTypeKind::Enum)
                enumOwner = receiver.Target()->Enum();
            else if (receiver.Target()->Kind() == SemanticTypeKind::Interface)
                interfaceOwner = receiver.Target()->Interface();
            if (receiver.Kind() == SemanticTypeKind::Pointer)
                RequireUnsafe(expression.Range());
        }
    }
    const auto* methods = owner       ? &owner->methods
                          : enumOwner ? &enumOwner->methods
                                      : nullptr;
    const std::string methodName =
        EffectName(expression.Member(), expression.IsCrashable());
    std::vector<const FunctionSymbol*> interfaceOverloads;
    if (interfaceOwner)
        for (const MethodContract* contract : interfaceOwner->methods)
            if (contract->key.name == expression.Member() &&
                contract->isCrashable == expression.IsCrashable() &&
                contract->callable)
                interfaceOverloads.push_back(contract->callable);
    if ((!methods || !methods->contains(methodName)) &&
        interfaceOverloads.empty()) {
        if (owner && expression.Member() == "tear") {
            Report(diagnostics::DiagnosticCode::InvalidLifecycle,
                   expression.MemberRange(),
                   "tear is compiler-only and cannot be called directly");
            return SemanticType::Error();
        }
        Report(diagnostics::DiagnosticCode::InvalidMember,
               expression.MemberRange(), "unknown method '" + methodName + "'");
        return SemanticType::Error();
    }
    const auto& overloads =
        interfaceOwner ? interfaceOverloads : methods->at(methodName);
    const FunctionSymbol* function =
        overloads.size() == 1
            ? overloads.front()
            : ResolveOverload(methodName, overloads, expression.Arguments(),
                              expression.Range(), isStatic, receiver);
    if (!function) return SemanticType::Error();
    _model._methods[&expression] = function;
    if (interfaceOwner)
        for (const MethodContract* contract : interfaceOwner->methods)
            if (contract->callable == function) {
                _model._interfaceMethods[&expression] = contract;
                break;
            }
    ValidateCrashableCall(expression.IsCrashable(), function->Signature(),
                          function, expression.Range());
    if (_currentFunction) _functionCalls[_currentFunction].insert(function);
    CheckMethodAccess(*function, expression.MemberRange());
    if (owner && function == owner->zero)
        WarnForNontrivialDefault(SemanticType(owner), expression.Range());
    if (owner && _inZero && function == owner->zero)
        RecordZeroDependency(SemanticType(owner));
    if (isStatic == function->HasReceiver())
        Report(diagnostics::DiagnosticCode::InvalidMember, expression.Range(),
               "instance methods need an instance; static methods use "
               "Type::method()");
    const std::size_t offset = function->HasReceiver() ? 1 : 0;
    if (offset && !isStatic) {
        const SemanticType expected = function->ParameterTypes()[0];
        const bool owningReceiver =
            expected.Kind() == SemanticTypeKind::Structure ||
            expected.Kind() == SemanticTypeKind::Enum;
        if (owningReceiver) {
            CheckTransfer(expression.Receiver(), expected);
            if (!IsAssignable(expected, receiver))
                Report(diagnostics::DiagnosticCode::IncompatibleArgument,
                       expression.Receiver().Range(),
                       TypeMismatch(expected, receiver));
        } else {
            SemanticType target =
                receiver.Kind() == SemanticTypeKind::Structure ||
                        receiver.Kind() == SemanticTypeKind::Enum
                    ? PlaceType(expression.Receiver())
                    : *receiver.Target();
            if (target.IsError())
                Report(diagnostics::DiagnosticCode::InvalidReference,
                       expression.Receiver().Range(),
                       "method receiver requires addressable storage");
            if (target.IsConst() && !expected.Target()->IsConst())
                Report(diagnostics::DiagnosticCode::SymbolNotWritable,
                       expression.Range(),
                       "cannot mutably borrow a const receiver");
            if (expected.Kind() == SemanticTypeKind::Pointer) {
                RequireUnsafe(expression.Range());
                if (receiver.Kind() == SemanticTypeKind::Reference)
                    Report(diagnostics::DiagnosticCode::ReferenceEscape,
                           expression.Range(),
                           "a checked reference cannot become a raw method "
                           "receiver");
            }
        }
    }
    std::vector<const core::Expression*> arguments;
    arguments.reserve(expression.Arguments().size());
    for (const auto& argument : expression.Arguments())
        arguments.push_back(argument.get());
    std::size_t required = function->ParameterTypes().size() - offset;
    if (const auto* declaration = function->Declaration()) {
        for (std::size_t index = offset;
             index < declaration->Parameters().size(); ++index)
            if (declaration->Parameters()[index]->DefaultValue() != nullptr) {
                required = index - offset;
                break;
            }
        if (arguments.size() >= required &&
            arguments.size() + offset <= declaration->Parameters().size()) {
            for (std::size_t index = arguments.size() + offset;
                 index < declaration->Parameters().size(); ++index)
                arguments.push_back(
                    declaration->Parameters()[index]->DefaultValue());
            if (arguments.size() != expression.Arguments().size())
                _model._callArguments[&expression] = arguments;
        }
    }
    if (arguments.size() + offset != function->ParameterTypes().size())
        Report(diagnostics::DiagnosticCode::WrongArgumentCount,
               expression.Range(),
               "method expects " +
                   (required != function->ParameterTypes().size() - offset
                        ? "between " + std::to_string(required) + " and " +
                              std::to_string(function->ParameterTypes().size() -
                                             offset)
                        : std::to_string(function->ParameterTypes().size() -
                                         offset)) +
                   " argument(s), found " +
                   std::to_string(expression.Arguments().size()));
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        const SemanticType expected =
            i + offset < function->ParameterTypes().size()
                ? function->ParameterTypes()[i + offset]
                : SemanticType::Error();
        const bool provided = i < expression.Arguments().size();
        const SemanticType actual =
            provided ? AnalyzeExpression(*arguments[i], expected)
                     : _model.TypeOf(*arguments[i]);
        if (provided)
            CheckTransfer(*arguments[i], expected,
                          i + offset < function->ParameterTypes().size() &&
                              function->ParameterContract(i + offset) ==
                                  core::OwnershipContract::Own);
        if (!IsAssignable(expected, actual) && !CanReborrow(expected, actual))
            Report(diagnostics::DiagnosticCode::IncompatibleArgument,
                   arguments[i]->Range(), TypeMismatch(expected, actual));
    }
    return function->ReturnType().WithoutConst();
}

}  // namespace flecha::semantic
