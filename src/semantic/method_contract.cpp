#include "semantic/method_contract.hpp"

#include <algorithm>
#include <string>
#include <utility>

#include "semantic/symbol.hpp"

namespace flecha::semantic {
namespace {

std::size_t ResultArity(SemanticType result) {
    if (result.Kind() == SemanticTypeKind::Void) return 0;
    if (result.Kind() == SemanticTypeKind::Tuple)
        return result.Elements().size();
    return 1;
}

std::string LocalName(const FunctionSymbol& function) {
    if (function.Declaration() != nullptr)
        return function.Declaration()->Name();
    std::string name = function.Name();
    if (function.IsCrashable() && name.ends_with('!')) name.pop_back();
    const std::size_t separator = name.rfind("::");
    if (separator != std::string::npos) name.erase(0, separator + 2);
    return name;
}

void AppendIdentity(std::string& result, std::string_view value) {
    result += std::to_string(value.size()) + ":" + std::string(value);
}

std::string NominalIdentity(SemanticType type) {
    if (type.Kind() == SemanticTypeKind::Structure) {
        const StructureType* structure = type.Structure();
        if (structure->syntax != nullptr &&
            !structure->syntax->specializationIdentity.empty())
            return structure->syntax->specializationIdentity;
        return structure->name;
    }
    if (type.Kind() == SemanticTypeKind::Enum) {
        const EnumType* enumeration = type.Enum();
        if (enumeration->syntax != nullptr &&
            !enumeration->syntax->specializationIdentity.empty())
            return enumeration->syntax->specializationIdentity;
        return enumeration->name;
    }
    if (type.Kind() == SemanticTypeKind::Interface)
        return type.Interface()->name;
    return type.Family().name;
}

}  // namespace

std::string CanonicalTypeIdentity(SemanticType type) {
    std::string result = "t" + std::to_string(static_cast<int>(type.Kind()));
    result += type.IsConst() ? "c" : "m";
    if (type.Kind() == SemanticTypeKind::Structure ||
        type.Kind() == SemanticTypeKind::Enum ||
        type.Kind() == SemanticTypeKind::Interface ||
        type.Kind() == SemanticTypeKind::NamedError) {
        result += "n";
        AppendIdentity(result, NominalIdentity(type));
    }
    if (type.Target() != nullptr) {
        result += "p";
        AppendIdentity(result, CanonicalTypeIdentity(*type.Target()));
    }
    if (type.Kind() == SemanticTypeKind::Array)
        result += "l" + std::to_string(type.Length());
    if (type.Kind() == SemanticTypeKind::Tuple) {
        result += "e" + std::to_string(type.Elements().size());
        for (const SemanticType element : type.Elements())
            AppendIdentity(result, CanonicalTypeIdentity(element));
    }
    if (type.Kind() == SemanticTypeKind::Function) {
        const CallableSignature& signature = type.Signature();
        result += "r";
        AppendIdentity(result, CanonicalTypeIdentity(signature.result));
        result += "a" + std::to_string(signature.parameters.size());
        for (std::size_t index = 0; index < signature.parameters.size();
             ++index) {
            AppendIdentity(result,
                           CanonicalTypeIdentity(signature.parameters[index]));
            result += "o" + std::to_string(static_cast<int>(
                                index < signature.parameterContracts.size()
                                    ? signature.parameterContracts[index]
                                    : core::OwnershipContract::None));
        }
    }
    return result;
}

VariadicShape NormalizeVariadicShape(
    std::vector<SemanticType> parameters,
    std::vector<core::OwnershipContract> parameterContracts) {
    parameterContracts.resize(parameters.size(), core::OwnershipContract::None);
    VariadicShape result{std::move(parameters), std::move(parameterContracts),
                         "v1"};
    result.key += "a" + std::to_string(result.parameters.size());
    for (std::size_t index = 0; index < result.parameters.size(); ++index) {
        AppendIdentity(result.key,
                       CanonicalTypeIdentity(result.parameters[index]));
        result.key +=
            "o" +
            std::to_string(static_cast<int>(result.parameterContracts[index]));
    }
    return result;
}

bool MethodContract::SameContract(const MethodContract& other) const {
    return key == other.key && result == other.result &&
           parameterContracts == other.parameterContracts &&
           resultContracts == other.resultContracts &&
           borrowSourceIndices == other.borrowSourceIndices &&
           borrowsVariadicPack == other.borrowsVariadicPack &&
           isUnsafe == other.isUnsafe && isOnce == other.isOnce &&
           isCrashable == other.isCrashable && visibility == other.visibility;
}

MethodContract NormalizeMethodContract(std::string name,
                                       CallableSignature signature,
                                       bool hasReceiver, bool hasVariadicPack,
                                       bool borrowsVariadicPack,
                                       MethodVisibility visibility,
                                       core::MethodCategory category) {
    const std::size_t parameterCount = signature.parameters.size();
    signature.parameterContracts.resize(parameterCount,
                                        core::OwnershipContract::None);
    signature.resultContracts.resize(ResultArity(signature.result),
                                     core::OwnershipContract::None);
    std::ranges::sort(signature.borrowSourceIndices);
    const auto duplicates = std::ranges::unique(signature.borrowSourceIndices);
    signature.borrowSourceIndices.erase(duplicates.begin(), duplicates.end());

    MethodContract result;
    result.key =
        NormalizeMethodKey(name, signature, hasReceiver, hasVariadicPack);
    result.result = signature.result;
    result.parameterContracts = std::move(signature.parameterContracts);
    if (hasReceiver && !result.parameterContracts.empty())
        result.parameterContracts.erase(result.parameterContracts.begin());
    result.resultContracts = std::move(signature.resultContracts);
    result.borrowSourceIndices = std::move(signature.borrowSourceIndices);
    result.borrowsVariadicPack = borrowsVariadicPack;
    result.isUnsafe = signature.isUnsafe;
    result.isOnce = signature.isOnce;
    result.isCrashable = signature.isCrashable;
    result.visibility = visibility;
    result.category = category;
    return result;
}

MethodContract MethodContractOf(const FunctionSymbol& function,
                                MethodVisibility visibility) {
    const bool hasVariadicPack = function.Declaration() != nullptr &&
                                 function.Declaration()->HasVariadicPack();
    return NormalizeMethodContract(LocalName(function), function.Signature(),
                                   function.HasReceiver(), hasVariadicPack,
                                   function.BorrowsVariadicPack(), visibility);
}

}  // namespace flecha::semantic
