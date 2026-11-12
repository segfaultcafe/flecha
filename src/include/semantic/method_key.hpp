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

// Canonical overload identity. Results, ownership, borrows, visibility, and
// call effects belong to MethodContract and deliberately do not create
// overloads.
struct MethodKey final {
    std::string name;
    MethodReceiverKind receiver = MethodReceiverKind::None;
    std::vector<SemanticType> parameters;
    bool hasVariadicPack = false;

    bool operator==(const MethodKey& other) const;
};

[[nodiscard]] MethodKey NormalizeMethodKey(std::string name,
                                           CallableSignature signature,
                                           bool hasReceiver,
                                           bool hasVariadicPack = false);

[[nodiscard]] MethodKey MethodKeyOf(const FunctionSymbol& function);

}  // namespace flecha::semantic

#endif  // FLECHA_SEMANTIC_METHOD_KEY_HPP
