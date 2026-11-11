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
           parameters == other.parameters &&
           hasVariadicPack == other.hasVariadicPack;
}

MethodKey NormalizeMethodKey(std::string name, CallableSignature signature,
                             bool hasReceiver, bool hasVariadicPack) {
    MethodKey result;
    result.name = std::move(name);
    result.hasVariadicPack = hasVariadicPack;

    if (hasReceiver) {
        assert(!signature.parameters.empty());
        result.receiver = ReceiverKind(signature.parameters.front());
        signature.parameters.erase(signature.parameters.begin());
    }
    result.parameters = std::move(signature.parameters);
    return result;
}

MethodKey MethodKeyOf(const FunctionSymbol& function) {
    const bool hasVariadicPack = function.Declaration() != nullptr &&
                                 function.Declaration()->HasVariadicPack();
    return NormalizeMethodKey(LocalName(function), function.Signature(),
                              function.HasReceiver(), hasVariadicPack);
}

}  // namespace flecha::semantic
