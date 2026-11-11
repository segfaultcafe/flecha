#include "semantic/method_contract.hpp"

#include <algorithm>
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

}  // namespace

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
