#ifndef FLECHA_SEMANTIC_METHOD_KEY_HPP
#define FLECHA_SEMANTIC_METHOD_KEY_HPP

#include <string>

#include "semantic/semantic_type.hpp"

namespace flecha::semantic {

class FunctionSymbol;

enum class MethodReceiverKind {
    None,
    Value,
    ConstValue,
    Reference,
    ConstReference,
    Pointer,
    ConstPointer,
};

// Canonical semantic identity for a method contract. The receiver's nominal
// owner is intentionally erased: an interface '&self' and a conforming
// structure '&self' have the same receiver kind even though their target
// types differ. All non-receiver parts of the callable signature remain exact.
struct MethodKey final {
    std::string name;
    MethodReceiverKind receiver = MethodReceiverKind::None;
    CallableSignature signature;
    bool borrowsReceiver = false;
    bool hasVariadicPack = false;
    bool borrowsVariadicPack = false;

    bool operator==(const MethodKey& other) const;
};

[[nodiscard]] MethodKey NormalizeMethodKey(std::string name,
                                           CallableSignature signature,
                                           bool hasReceiver,
                                           bool hasVariadicPack = false,
                                           bool borrowsVariadicPack = false);

[[nodiscard]] MethodKey MethodKeyOf(const FunctionSymbol& function);

}  // namespace flecha::semantic

#endif  // FLECHA_SEMANTIC_METHOD_KEY_HPP
