#include "semantic/semantic_analyzer.hpp"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "core/output_format.hpp"
#include "core/token_type.hpp"
#include "semantic/allocation_checker.hpp"
#include "semantic/borrow_checker.hpp"
#include "semantic/definite_assignment.hpp"
#include "semantic/generic_expander.hpp"

namespace flecha::semantic {
namespace {

std::string TypeMismatch(SemanticType expected, SemanticType actual) {
    return "expected '" + std::string(expected.Name()) + "', found '" +
           std::string(actual.Name()) + "'";
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

bool IsRuntimeArrayStructureField(SemanticType type) {
    if (type.IsNumeric() || type.Kind() == SemanticTypeKind::Bool ||
        type.Kind() == SemanticTypeKind::Char ||
        type.Kind() == SemanticTypeKind::Pointer)
        return true;
    if (type.Kind() == SemanticTypeKind::Enum) return type.IsZeroable();
    if (type.Kind() == SemanticTypeKind::Array)
        return IsRuntimeArrayStructureField(*type.Target());
    if (type.Kind() == SemanticTypeKind::Structure) {
        if (!type.IsCopyable() || !type.IsZeroable()) return false;
        for (const auto& field : type.Structure()->fields)
            if (!IsRuntimeArrayStructureField(field.type)) return false;
        return true;
    }
    return false;
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
        const auto result =
            std::from_chars(text.data(), text.data() + text.size(), value);
        if (result.ec == std::errc{} && result.ptr == text.data() + text.size())
            return value;
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
                    name = function.Name();
                } else if (declaration->Kind() ==
                           core::AstNodeKind::StructureDeclaration)
                    name = static_cast<const core::StructureDeclaration&>(
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
            if (found.size() > 1) {
                _ambiguousImports[module.sourceId].insert_or_assign(name,
                                                                    found);
                continue;
            }
            const Symbol* symbol = _model.DeclaredSymbol(*found.front());
            if (symbol) (void)scope->DeclareAs(name, *symbol);
        }
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
    for (const auto& structure : _model._structures) {
        if (!structure->syntax->exported && !structure->syntax->quiverVisible)
            continue;
        for (const auto& field : structure->fields)
            if (!field.syntax->IsPrivate())
                check(field.type, field.syntax->Range(), *structure->syntax);
        for (const auto& [name, method] : structure->methods) {
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
        for (const auto& [name, method] : enumeration->methods) {
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
        if (function.Owner().empty()) localName = function.Name();
    }
    Scope* declarationScope = _currentScope;
    const bool topLevel =
        declaration.Kind() == core::AstNodeKind::FunctionDeclaration ||
        declaration.Kind() == core::AstNodeKind::StructureDeclaration ||
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
    const bool declared = declarationScope->DeclareAs(localName, *result);
    if (localName != result->Name()) (void)declarationScope->Declare(*result);
    if (!declared) {
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
                                  symbol->Kind() != SymbolKind::CallableType)) {
            if (!ReportInaccessibleImport(type.Name(), type.Range()))
                Report(diagnostics::DiagnosticCode::UnknownType, type.Range(),
                       "unknown type '" + type.Name() + "'");
            return SemanticType::Error();
        }
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
    auto family = std::make_shared<ErrorFamily>();
    family->name = "AllocationError";
    family->variants = {"OutOfMemory"};
    auto error = std::make_unique<Symbol>(SymbolKind::ErrorType, family->name,
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
                const auto parsed = std::from_chars(
                    digits.data(), digits.data() + digits.size(), magnitude);
                valid = parsed.ec == std::errc{} &&
                        parsed.ptr == digits.data() + digits.size();
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
    StructureType* owner = nullptr;
    EnumType* enumOwner = nullptr;
    if (!function.Owner().empty()) {
        const Symbol* type = _currentScope->Lookup(function.Owner());
        if (!type || (type->Kind() != SymbolKind::StructureType &&
                      type->Kind() != SymbolKind::EnumType)) {
            Report(diagnostics::DiagnosticCode::UnknownType,
                   function.NameRange(),
                   "unknown structure or enum '" + function.Owner() + "'");
        } else if (type->Kind() == SymbolKind::StructureType)
            owner = const_cast<StructureType*>(type->Type().Structure());
        else
            enumOwner = const_cast<EnumType*>(type->Type().Enum());
    }
    std::vector<SemanticType> parameterTypes;
    std::vector<core::OwnershipContract> parameterContracts;
    bool receiver = false;
    for (const auto& parameter : function.Parameters()) {
        SemanticType type = ResolveType(parameter->Type());
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
                 receiverTarget.Enum() == enumOwner);
            receiver = (owner || enumOwner) && parameterTypes.empty() &&
                       (type.Kind() == SemanticTypeKind::Structure ||
                        type.Kind() == SemanticTypeKind::Enum ||
                        type.Kind() == SemanticTypeKind::Reference ||
                        type.Kind() == SemanticTypeKind::Pointer) &&
                       matchingOwner;
            if (!receiver)
                Report(diagnostics::DiagnosticCode::InvalidMember,
                       parameter->Range(),
                       "self must be the first parameter and own, borrow, or "
                       "point to the owning structure or enum");
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
               function.BorrowSourceRanges().front(),
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
    const bool lifecycle =
        owner && (function.Name() == "init" || function.Name() == "zero" ||
                  function.Name() == "tear");
    if (function.HasModifier(core::FunctionModifier::Override) !=
        (lifecycle && !generated))
        Report(diagnostics::DiagnosticCode::InvalidLifecycle,
               function.NameRange(),
               "override is required exactly when replacing init, zero, or "
               "tear");
    if (function.HasModifier(core::FunctionModifier::Private) && !owner &&
        !enumOwner)
        Report(diagnostics::DiagnosticCode::InvalidMember, function.NameRange(),
               "private requires a structure or enum method");
    if (owner && function.Name() == "init" &&
        (receiver || returnType.WithoutConst() != SemanticType(owner)))
        Report(diagnostics::DiagnosticCode::InvalidLifecycle, function.Range(),
               "init must be static and return its structure type");
    if (owner && function.Name() == "zero" &&
        (function.IsUnsafe() || receiver || !parameterTypes.empty() ||
         returnType.WithoutConst() != SemanticType(owner)))
        Report(diagnostics::DiagnosticCode::InvalidLifecycle, function.Range(),
               "zero must be safe, static, take no parameters, and return its "
               "structure type");
    if (owner && function.Name() == "zero" &&
        function.HasModifier(core::FunctionModifier::Private))
        Report(diagnostics::DiagnosticCode::InvalidLifecycle, function.Range(),
               "zero cannot be private because structure default "
               "construction is part of the type's API");
    if (owner && function.Name() == "tear" &&
        (returnType.Kind() != SemanticTypeKind::Void ||
         parameterTypes.size() != 1 || !receiver ||
         parameterTypes[0].Kind() != SemanticTypeKind::Reference ||
         parameterTypes[0].Target()->IsConst()))
        Report(diagnostics::DiagnosticCode::InvalidLifecycle, function.Range(),
               "tear must return void and take only &self");
    if (enumOwner && (function.Name() == "init" || function.Name() == "zero" ||
                      function.Name() == "tear"))
        Report(diagnostics::DiagnosticCode::InvalidLifecycle, function.Range(),
               "enum lifecycle is compiler-defined; init, zero, and tear are "
               "reserved");
    if ((owner || enumOwner) && function.exported)
        Report(diagnostics::DiagnosticCode::InvalidMember, function.Range(),
               "export the owning type, not individual member functions");
    if ((owner || enumOwner) && function.quiverVisible)
        Report(diagnostics::DiagnosticCode::InvalidMember, function.Range(),
               "member functions cannot be marked quiver; visibility comes "
               "from the owning type");
    if (owner && owner->syntax->moduleId != function.moduleId)
        Report(diagnostics::DiagnosticCode::PrivateAccess, function.Range(),
               "methods must be defined in their structure's module");
    if (enumOwner && enumOwner->syntax->moduleId != function.moduleId)
        Report(diagnostics::DiagnosticCode::PrivateAccess, function.Range(),
               "methods must be defined in their enum's module");
    const std::string name = owner ? owner->name + "::" + function.Name()
                             : enumOwner
                                 ? enumOwner->name + "::" + function.Name()
                                 : TopName(function, function.Name());
    auto signature = std::make_shared<const CallableSignature>(
        CallableSignature{returnType, parameterTypes, parameterContracts,
                          ResultContracts(function.ReturnType()),
                          borrowSourceIndices, function.IsUnsafe(),
                          function.HasModifier(core::FunctionModifier::Once)});
    const auto* symbol = static_cast<const FunctionSymbol*>(
        AddSymbol(std::make_unique<FunctionSymbol>(
                      name, std::move(signature), function.NameRange(),
                      BuiltInFunction::None, owner,
                      function.HasModifier(core::FunctionModifier::Private),
                      receiver, function.IsExtern(),
                      function.IsExtern() ? function.Name() : "", enumOwner),
                  function));
    if (symbol && function.HasModifier(core::FunctionModifier::Extern))
        _forbiddenZeroEffects.insert(symbol);
    if (owner && symbol) {
        owner->methods.emplace(function.Name(), symbol);
        if (function.Name() == "init") {
            owner->init = symbol;
            owner->hasCustomInit = !generated;
        }
        if (function.Name() == "zero") {
            owner->zero = symbol;
            owner->zeroIsGenerated = generated;
        }
        if (function.Name() == "tear") owner->tear = symbol;
    }
    if (enumOwner && symbol) {
        if (!enumOwner->methods.emplace(function.Name(), symbol).second)
            Report(diagnostics::DiagnosticCode::DuplicateDeclaration,
                   function.NameRange(),
                   "duplicate enum method '" + function.Name() + "'");
        for (const EnumVariant& variant : enumOwner->variants)
            if (variant.name == function.Name())
                Report(diagnostics::DiagnosticCode::DuplicateDeclaration,
                       function.NameRange(),
                       "method conflicts with enum variant '" + variant.name +
                           "'");
    }
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
    _forbiddenZeroEffects.clear();
    _buildSelf = nullptr;
    _allowBuildSelf = false;
    _buildDepth = 0;
    _analyzingCallCallee = false;
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
                if (name == import.alias)
                    Report(diagnostics::DiagnosticCode::DuplicateDeclaration,
                           import.range,
                           "import alias conflicts with declaration '" + name +
                               "'");
            }
    }
    const auto bindGenericAliases = [&] {
        for (const auto& [alias, declaration] : unit.genericAliases)
            if (const auto* symbol = _model.DeclaredSymbol(*declaration))
                (void)_model._scopes.front()->DeclareAs(alias, *symbol);
    };
    RegisterErrorTypes(unit);
    RegisterEnums(unit);
    RegisterStructures(unit);
    DeclareCallableTypes(unit);
    RegisterModuleConstants(unit);
    bindGenericAliases();
    BindImports(unit);
    ResolveStructureFields();
    ResolveCallableTypes(unit);
    RegisterFunctions(unit);
    bindGenericAliases();
    BindImports(unit);
    AnalyzeModuleConstants(unit);
    GenerateInitializers();
    BindImports(unit);
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
        AnalyzeFunction(*function);
    for (const std::unique_ptr<core::Declaration>& declaration :
         unit.Declarations()) {
        if (declaration->Kind() == core::AstNodeKind::FunctionDeclaration) {
            AnalyzeFunction(
                static_cast<const core::FunctionDeclaration&>(*declaration));
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
    for (const auto& structure : _model._structures)
        if (SemanticType(structure.get()).IsZeroable())
            WarnForNontrivialDefault(SemanticType(structure.get()),
                                     structure->syntax->NameRange());
    ValidateZeroCycles();
    ValidateZeroEffects();
    if (!_diagnostics.HasErrors())
        AnalyzeAllocations(unit, _model, _diagnostics);
    ReportGenericOrigins(unit, _diagnostics);
    _currentScope = nullptr;
    _currentFunction = nullptr;
    return std::move(_model);
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
                for (std::size_t index = 0; index < call.Arguments().size();
                     ++index) {
                    const SemanticType type =
                        signature != nullptr &&
                                (!function || function->BuiltIn() ==
                                                  BuiltInFunction::None) &&
                                index < signature->parameters.size()
                            ? signature->parameters[index]
                            : _model.TypeOf(*call.Arguments()[index]);
                    operands.emplace_back(call.Arguments()[index].get(), type);
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
                for (std::size_t index = 0; index < call.Arguments().size();
                     ++index) {
                    const SemanticType type =
                        index + offset < function->ParameterTypes().size()
                            ? function->ParameterTypes()[index + offset]
                            : _model.TypeOf(*call.Arguments()[index]);
                    operands.emplace_back(call.Arguments()[index].get(), type);
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
                 function->BuiltIn() == BuiltInFunction::StorageFree)) {
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
            if (!source.IsError() && !source.IsSequence())
                Report(diagnostics::DiagnosticCode::IncompatibleType,
                       range.Source().Range(),
                       "range requires a fixed array or slice value");
            if (range.IndexBinding() != nullptr) {
                const Symbol* symbol =
                    _model.DeclaredSymbol(*range.IndexBinding());
                if (symbol != nullptr && !symbol->Type().IsError()) {
                    const SemanticType index = symbol->Type();
                    if (!index.IsInteger()) {
                        Report(diagnostics::DiagnosticCode::IncompatibleType,
                               range.IndexBinding()->NameRange(),
                               "range index must have an integer type");
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
            if (symbol->Kind() == kind && symbol->Type() == type)
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
            missing += enumeration->name + "." + variant.name;
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
        const auto parsed =
            std::from_chars(text.data(), text.data() + text.size(), value);
        if (parsed.ec != std::errc{} ||
            parsed.ptr != text.data() + text.size() || value > limit ||
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
                    return expected.IsNumeric() && literal.Value() == "0";
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
    if (_currentFunction && target.Kind() == SemanticTypeKind::String &&
        source.Kind() != SemanticTypeKind::String)
        _forbiddenZeroEffects.insert(_currentFunction);
    return target.WithoutConst();
}

SemanticType SemanticAnalyzer::AnalyzeName(
    const core::NameExpression& expression) {
    if (ReportAmbiguousImport(expression.Name(), expression.Range()))
        return SemanticType::Error();
    const Symbol* symbol = _currentScope->Lookup(expression.Name());
    if (symbol == nullptr) {
        if (!ReportInaccessibleImport(expression.Name(), expression.Range()))
            Report(diagnostics::DiagnosticCode::UnknownName, expression.Range(),
                   "unknown name '" + expression.Name() + "'");
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
                   "enum '" + symbol->Name() + "' has no variant '" +
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
               "error type '" + symbol->Name() + "' has no variant '" +
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
        if (same && left.IsNumeric()) {
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
                !(element.IsNumeric() ||
                  element.Kind() == SemanticTypeKind::Bool ||
                  element.Kind() == SemanticTypeKind::Char ||
                  (element.Kind() == SemanticTypeKind::Enum &&
                   element.IsZeroable()) ||
                  (element.Kind() == SemanticTypeKind::Structure &&
                   IsRuntimeArrayStructureField(element))))
                Report(diagnostics::DiagnosticCode::InvalidAllocation,
                       expression.TypeArgument()->Range(),
                       "runtime arrays require numeric, bool, char, enum, or "
                       "trivially copyable default-constructible structure "
                       "elements with supported fields");
            if (dynamic && !uninitialized &&
                element.Kind() == SemanticTypeKind::Structure &&
                IsRuntimeArrayStructureField(element))
                WarnForNontrivialDefault(element,
                                         expression.TypeArgument()->Range());
            if (uninitialized && element.IsImmovable())
                Report(diagnostics::DiagnosticCode::InvalidAllocation,
                       expression.TypeArgument()->Range(),
                       "uninitialized collection storage requires movable "
                       "elements");
            if (!element.IsError() && !StorageSize(element))
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
        result = SemanticType(std::vector<SemanticType>{
            SemanticType(dynamic ? (safeOwner ? SemanticTypeKind::OwnedSlice
                                              : SemanticTypeKind::OwnedBuffer)
                                 : SemanticTypeKind::Pointer,
                         element),
            SemanticType(SemanticTypeKind::ErrorValue)});
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
    auto specialized = std::make_unique<FunctionSymbol>(
        callee.Name(), result, std::vector<SemanticType>{parameter},
        callee.Range(), intrinsic, true);
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
    const bool previousCallee = _analyzingCallCallee;
    _analyzingCallCallee = true;
    const SemanticType calleeType = AnalyzeExpression(expression.Callee());
    _analyzingCallCallee = previousCallee;
    const FunctionSymbol* function = nullptr;
    if (expression.Callee().Kind() == core::AstNodeKind::NameExpression) {
        const Symbol* symbol = _model.SymbolFor(
            static_cast<const core::NameExpression&>(expression.Callee()));
        if (symbol != nullptr && symbol->Kind() == SymbolKind::Function) {
            function = static_cast<const FunctionSymbol*>(symbol);
        }
    }
    if (function && (function->BuiltIn() == BuiltInFunction::New ||
                     function->BuiltIn() == BuiltInFunction::Free ||
                     function->BuiltIn() == BuiltInFunction::StorageNew ||
                     function->BuiltIn() == BuiltInFunction::StorageFree))
        return AnalyzeAllocationCall(expression, function->BuiltIn());
    if (function && function->BuiltIn() == BuiltInFunction::Hash)
        return AnalyzeHashCall(expression);
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
    std::vector<SemanticType> argumentTypes;
    for (std::size_t index = 0; index < expression.Arguments().size();
         ++index) {
        const SemanticType expected =
            signature != nullptr &&
                    (!function ||
                     function->BuiltIn() == BuiltInFunction::None) &&
                    index < signature->parameters.size()
                ? signature->parameters[index]
                : SemanticType::Error();
        argumentTypes.push_back(
            AnalyzeExpression(*expression.Arguments()[index], expected));
        const bool consuming = signature != nullptr &&
                               index < signature->parameterContracts.size() &&
                               signature->parameterContracts[index] ==
                                   core::OwnershipContract::Own;
        CheckTransfer(*expression.Arguments()[index], expected, consuming);
    }
    if (calleeType.IsError()) return SemanticType::Error();
    if (signature == nullptr) {
        Report(diagnostics::DiagnosticCode::NotCallable,
               expression.Callee().Range(), "expression is not callable");
        return SemanticType::Error();
    }
    if (signature->isUnsafe || signature->result.IsImmovable())
        RequireUnsafe(expression.Callee().Range());
    if (_currentFunction && function)
        _functionCalls[_currentFunction].insert(function);
    if (function && (function->BuiltIn() == BuiltInFunction::Stdout ||
                     function->BuiltIn() == BuiltInFunction::StdoutLine ||
                     function->BuiltIn() == BuiltInFunction::StdoutFormat)) {
        if (_inZero)
            Report(diagnostics::DiagnosticCode::InvalidLifecycle,
                   expression.Range(),
                   "zero cannot cause external side effects");
        if (_currentFunction) _forbiddenZeroEffects.insert(_currentFunction);
        if (function->BuiltIn() != BuiltInFunction::StdoutFormat) {
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
            Report(diagnostics::DiagnosticCode::WrongArgumentCount,
                   expression.Range(),
                   "function 'stdoutf' requires a format string");
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
                   "stdoutf format must be a string literal");
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
            bool valid = type.IsError();
            std::string_view expected;
            char specifier = '?';
            switch (part.kind) {
                case core::OutputFormatPartKind::Decimal:
                    valid = valid || type.IsNumeric();
                    expected = "a number";
                    specifier = 'd';
                    break;
                case core::OutputFormatPartKind::String:
                    valid = valid || type.Kind() == SemanticTypeKind::String ||
                            type.Kind() == SemanticTypeKind::StringView ||
                            type.Kind() == SemanticTypeKind::Char;
                    expected = "a string or char";
                    specifier = 's';
                    break;
                case core::OutputFormatPartKind::Boolean:
                    valid = valid || type.Kind() == SemanticTypeKind::Bool ||
                            type.Kind() == SemanticTypeKind::U8;
                    expected = "a bool or u8";
                    specifier = 'b';
                    break;
                case core::OutputFormatPartKind::Literal:
                    break;
            }
            if (!valid)
                Report(diagnostics::DiagnosticCode::IncompatibleArgument,
                       expression.Arguments()[argumentIndex]->Range(),
                       "format argument " + std::to_string(argumentIndex) +
                           " for '%" + std::string(1, specifier) +
                           "' requires " + std::string(expected) + ", found '" +
                           std::string(type.Name()) + "'");
            ++argumentIndex;
        }
        return function->ReturnType().WithoutConst();
    }
    if (argumentTypes.size() != signature->parameters.size()) {
        Report(
            diagnostics::DiagnosticCode::WrongArgumentCount, expression.Range(),
            std::string(function ? "function '" + function->Name() + "'"
                                 : "callable") +
                " expects " + std::to_string(signature->parameters.size()) +
                " argument(s), found " + std::to_string(argumentTypes.size()));
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
                   expression.Arguments()[index]->Range(),
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
        return AnalyzeStructureCall(expression, SemanticType::Error());
    const SemanticType receiver = AnalyzeExpression(expression.Receiver());
    if (receiver.Kind() == SemanticTypeKind::Structure ||
        receiver.Kind() == SemanticTypeKind::Enum ||
        ((receiver.Kind() == SemanticTypeKind::Pointer ||
          receiver.Kind() == SemanticTypeKind::Reference) &&
         (receiver.Target()->Kind() == SemanticTypeKind::Structure ||
          receiver.Target()->Kind() == SemanticTypeKind::Enum)))
        return AnalyzeStructureCall(expression, receiver);
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

void SemanticAnalyzer::RegisterStructures(const core::CompilationUnit& unit) {
    for (const auto& declaration : unit.Declarations()) {
        if (declaration->Kind() != core::AstNodeKind::StructureDeclaration)
            continue;
        const auto& syntax =
            static_cast<const core::StructureDeclaration&>(*declaration);
        SetSource(syntax.sourceId);
        auto structure = std::make_unique<StructureType>();
        structure->name = TopName(syntax, syntax.Name());
        structure->displayName = syntax.specializationName;
        structure->syntax = &syntax;
        AddSymbol(std::make_unique<Symbol>(
                      SymbolKind::StructureType, TopName(syntax, syntax.Name()),
                      SemanticType(structure.get()), syntax.NameRange(), false),
                  syntax);
        _model._structures.push_back(std::move(structure));
    }
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
        for (const auto& field : structure->fields)
            (void)ResolveType(field.syntax->Type());
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
        if (structure->init) continue;
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
                   type.Kind() == SemanticTypeKind::Pointer ||
                   type.Kind() == SemanticTypeKind::OwnedBuffer ||
                   type.Kind() == SemanticTypeKind::OwnedSlice;
        const StructureType* nested = type.Structure();
        if (nested->zero) return true;
        if (const auto found = zeroable.find(nested); found != zeroable.end())
            return found->second;
        if (nested->hasCustomInit || nested->tear ||
            !checking.insert(nested).second)
            return false;
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
    if (function.IsUnsafe() || function.ReturnType().IsImmovable())
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
    std::function<void(SemanticType)> checkDefault = [&](SemanticType field) {
        if (field.Kind() == SemanticTypeKind::Enum && !field.IsZeroable())
            Report(diagnostics::DiagnosticCode::InvalidStructure,
                   expression.Range(),
                   "field of enum type '" + field.Name() +
                       "' has no default variant");
        if (field.Kind() == SemanticTypeKind::Structure) {
            const FunctionSymbol* method =
                zeroBuild ? field.Structure()->zero : field.Structure()->init;
            if (!method && zeroBuild)
                Report(diagnostics::DiagnosticCode::InvalidStructure,
                       expression.Range(),
                       "field of type '" + field.Name() +
                           "' has no valid default construction");
            if (method) {
                CheckMethodAccess(*method, expression.Range());
                if (_currentFunction)
                    _functionCalls[_currentFunction].insert(method);
            }
            if (method && !method->ParameterTypes().empty())
                Report(diagnostics::DiagnosticCode::InvalidStructure,
                       expression.Range(),
                       "field of type '" + field.Name() +
                           "' needs an explicit declaration initializer "
                           "because its init requires arguments");
        }
        if (field.Kind() == SemanticTypeKind::Array)
            checkDefault(*field.Target());
        if (field.Kind() == SemanticTypeKind::Tuple)
            for (const auto& element : field.Elements()) checkDefault(element);
    };
    for (const auto& field : type.Structure()->fields)
        if (zeroBuild || !field.syntax->Initializer()) checkDefault(field.type);
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

SemanticType SemanticAnalyzer::AnalyzeStructureCall(
    const core::MemberCallExpression& expression, SemanticType receiver) {
    const bool isStatic = expression.Access() == TokenType::Scope;
    const StructureType* owner = nullptr;
    const EnumType* enumOwner = nullptr;
    if (isStatic) {
        if (expression.Receiver().Kind() == core::AstNodeKind::NameExpression) {
            const Symbol* type = _currentScope->Lookup(
                static_cast<const core::NameExpression&>(expression.Receiver())
                    .Name());
            if (type && type->Kind() == SymbolKind::StructureType)
                owner = type->Type().Structure();
            if (type && type->Kind() == SymbolKind::EnumType)
                enumOwner = type->Type().Enum();
        }
    } else {
        if (receiver.Kind() == SemanticTypeKind::Structure ||
            receiver.Kind() == SemanticTypeKind::Enum) {
            if (expression.Access() != TokenType::Dot)
                Report(diagnostics::DiagnosticCode::InvalidMember,
                       expression.Range(), "value receivers use '.'");
            if (receiver.Kind() == SemanticTypeKind::Structure)
                owner = receiver.Structure();
            else
                enumOwner = receiver.Enum();
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
            if (receiver.Kind() == SemanticTypeKind::Pointer)
                RequireUnsafe(expression.Range());
        }
    }
    const auto* methods = owner       ? &owner->methods
                          : enumOwner ? &enumOwner->methods
                                      : nullptr;
    if (!methods || !methods->contains(expression.Member())) {
        if (owner && expression.Member() == "tear") {
            Report(diagnostics::DiagnosticCode::InvalidLifecycle,
                   expression.MemberRange(),
                   "tear is compiler-only and cannot be called directly");
            return SemanticType::Error();
        }
        Report(diagnostics::DiagnosticCode::InvalidMember,
               expression.MemberRange(),
               "unknown method '" + expression.Member() + "'");
        return SemanticType::Error();
    }
    const FunctionSymbol* function = methods->at(expression.Member());
    _model._methods[&expression] = function;
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
    if (expression.Arguments().size() + offset !=
        function->ParameterTypes().size())
        Report(diagnostics::DiagnosticCode::WrongArgumentCount,
               expression.Range(), "wrong number of method arguments");
    for (std::size_t i = 0; i < expression.Arguments().size(); ++i) {
        const SemanticType expected =
            i + offset < function->ParameterTypes().size()
                ? function->ParameterTypes()[i + offset]
                : SemanticType::Error();
        const SemanticType actual =
            AnalyzeExpression(*expression.Arguments()[i], expected);
        CheckTransfer(*expression.Arguments()[i], expected,
                      i + offset < function->ParameterTypes().size() &&
                          function->ParameterContract(i + offset) ==
                              core::OwnershipContract::Own);
        if (!IsAssignable(expected, actual) && !CanReborrow(expected, actual))
            Report(diagnostics::DiagnosticCode::IncompatibleArgument,
                   expression.Arguments()[i]->Range(),
                   TypeMismatch(expected, actual));
    }
    return function->ReturnType().WithoutConst();
}

}  // namespace flecha::semantic
