#include "semantic/method_key.hpp"

#include <algorithm>
#include <cassert>
#include <utility>

#include "semantic/symbol.hpp"

namespace flecha::semantic {
namespace {

MethodReceiverKind ReceiverKind(SemanticType receiver) {
    if (receiver.Kind() == SemanticTypeKind::Reference) {
        assert(receiver.Target() != nullptr);
        return receiver.IsConst() || receiver.Target()->IsConst()
                   ? MethodReceiverKind::ConstReference
                   : MethodReceiverKind::Reference;
    }
    if (receiver.Kind() == SemanticTypeKind::Pointer) {
        assert(receiver.Target() != nullptr);
        return receiver.IsConst() || receiver.Target()->IsConst()
                   ? MethodReceiverKind::ConstPointer
                   : MethodReceiverKind::Pointer;
    }
    assert(receiver.Kind() == SemanticTypeKind::Structure ||
           receiver.Kind() == SemanticTypeKind::Enum ||
           receiver.Kind() == SemanticTypeKind::Interface);
    return receiver.IsConst() ? MethodReceiverKind::ConstValue
                              : MethodReceiverKind::Value;
}

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

bool MethodKey::operator==(const MethodKey& other) const {
    return name == other.name && receiver == other.receiver &&
           signature == other.signature &&
           borrowsReceiver == other.borrowsReceiver &&
           hasVariadicPack == other.hasVariadicPack &&
           borrowsVariadicPack == other.borrowsVariadicPack;
}

MethodKey NormalizeMethodKey(std::string name, CallableSignature signature,
                             bool hasReceiver, bool hasVariadicPack,
                             bool borrowsVariadicPack) {
    MethodKey result;
    result.name = std::move(name);
    result.hasVariadicPack = hasVariadicPack;
    result.borrowsVariadicPack = borrowsVariadicPack;

    const std::size_t parameterCount = signature.parameters.size();
    signature.parameterContracts.resize(parameterCount,
                                        core::OwnershipContract::None);
    signature.resultContracts.resize(ResultArity(signature.result),
                                     core::OwnershipContract::None);

    std::vector<std::size_t> borrowSources =
        std::move(signature.borrowSourceIndices);
    signature.borrowSourceIndices.clear();

    if (hasReceiver) {
        assert(!signature.parameters.empty());
        result.receiver = ReceiverKind(signature.parameters.front());
        signature.parameters.erase(signature.parameters.begin());
        signature.parameterContracts.erase(
            signature.parameterContracts.begin());
    }

    for (const std::size_t source : borrowSources) {
        assert(source < parameterCount);
        if (hasReceiver && source == 0) {
            result.borrowsReceiver = true;
            continue;
        }
        signature.borrowSourceIndices.push_back(source - (hasReceiver ? 1 : 0));
    }
    std::ranges::sort(signature.borrowSourceIndices);
    const auto duplicate = std::ranges::unique(signature.borrowSourceIndices);
    signature.borrowSourceIndices.erase(duplicate.begin(), duplicate.end());

    result.signature = std::move(signature);
    return result;
}

MethodKey MethodKeyOf(const FunctionSymbol& function) {
    const bool hasVariadicPack = function.Declaration() != nullptr &&
                                 function.Declaration()->HasVariadicPack();
    return NormalizeMethodKey(LocalName(function), function.Signature(),
                              function.HasReceiver(), hasVariadicPack,
                              function.BorrowsVariadicPack());
}

}  // namespace flecha::semantic
