#ifndef FLECHA_SEMANTIC_METHOD_CONTRACT_HPP
#define FLECHA_SEMANTIC_METHOD_CONTRACT_HPP

#include <string>
#include <vector>

#include "semantic/method_key.hpp"

namespace flecha::semantic {

struct InterfaceType;

enum class MethodVisibility { Public, Protected };

// A variadic interface family is materialized once for every closed argument
// vector reached by the whole source graph. The key is canonical and stable:
// display aliases and discovery order never participate in slot identity.
struct VariadicShape final {
    std::vector<SemanticType> parameters;
    std::vector<core::OwnershipContract> parameterContracts;
    std::string key;

    [[nodiscard]] bool operator==(const VariadicShape& other) const {
        return key == other.key && parameters == other.parameters &&
               parameterContracts == other.parameterContracts;
    }
};

[[nodiscard]] std::string CanonicalTypeIdentity(SemanticType type);
[[nodiscard]] VariadicShape NormalizeVariadicShape(
    std::vector<SemanticType> parameters,
    std::vector<core::OwnershipContract> parameterContracts);

struct MethodContract final {
    MethodKey key;
    SemanticType result{SemanticTypeKind::Void};
    std::vector<core::OwnershipContract> parameterContracts;
    std::vector<core::OwnershipContract> resultContracts;
    std::vector<std::size_t> borrowSourceIndices;
    bool borrowsVariadicPack = false;
    bool isUnsafe = false;
    bool isOnce = false;
    bool isCrashable = false;
    MethodVisibility visibility = MethodVisibility::Public;
    core::MethodCategory category = core::MethodCategory::Missing;
    const InterfaceType* declaredBy = nullptr;
    const core::InterfaceMethod* syntax = nullptr;
    const MethodContract* inherited = nullptr;
    const FunctionSymbol* callable = nullptr;
    const FunctionSymbol* provider = nullptr;

    [[nodiscard]] bool SameContract(const MethodContract& other) const;
};

[[nodiscard]] MethodContract NormalizeMethodContract(
    std::string name, CallableSignature signature, bool hasReceiver,
    bool hasVariadicPack, bool borrowsVariadicPack,
    MethodVisibility visibility = MethodVisibility::Public,
    core::MethodCategory category = core::MethodCategory::Missing);

[[nodiscard]] MethodContract MethodContractOf(
    const FunctionSymbol& function,
    MethodVisibility visibility = MethodVisibility::Public);

}  // namespace flecha::semantic

#endif  // FLECHA_SEMANTIC_METHOD_CONTRACT_HPP
