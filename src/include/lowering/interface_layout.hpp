#ifndef FLECHA_LOWERING_INTERFACE_LAYOUT_HPP
#define FLECHA_LOWERING_INTERFACE_LAYOUT_HPP

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "semantic/semantic_model.hpp"

namespace flecha::lowering {

struct CanonicalInterfaceSlot final {
    const semantic::MethodContract* origin = nullptr;
    const semantic::MethodContract* effective = nullptr;
};

struct SlotLayout final {
    CanonicalInterfaceSlot slot;
    std::size_t offset = 0;
};

struct InterfaceTableLayout;

// Parent subtables are recursive because their canonical slots keep the
// effective declaration selected by this root, not by the parent in isolation.
struct ParentLayout final {
    const semantic::InterfaceType* interfaceType = nullptr;
    std::size_t offset = 0;
    std::shared_ptr<const InterfaceTableLayout> table;
};

struct InterfaceTableLayout final {
    const semantic::InterfaceType* interfaceType = nullptr;
    std::size_t size = 0;
    std::size_t alignment = 1;
    std::vector<ParentLayout> parents;
    std::vector<SlotLayout> slots;
};

enum class ConcreteTypeKind { Structure, Enum };

struct ConcreteInterfaceType final {
    ConcreteTypeKind kind = ConcreteTypeKind::Structure;
    const semantic::StructureType* structure = nullptr;
    const semantic::EnumType* enumeration = nullptr;

    [[nodiscard]] std::string DisplayName() const;
};

enum class InterfaceProviderKind {
    // A concrete receiver adapter will be required at emission time.
    ConcreteThunk,
    // An interface body needs a table-pointer adjustment before it can run.
    AdjustmentThunk,
    // The interface body's expected table view already has the same address.
    InterfaceBody,
};

struct EffectiveInterfaceProvider final {
    CanonicalInterfaceSlot slot;
    const semantic::FunctionSymbol* function = nullptr;
    InterfaceProviderKind kind = InterfaceProviderKind::ConcreteThunk;
    std::ptrdiff_t tableAdjustment = 0;
};

struct InterfaceProviderMap final {
    ConcreteInterfaceType concrete;
    const semantic::InterfaceType* root = nullptr;
    std::vector<EffectiveInterfaceProvider> providers;

    [[nodiscard]] const EffectiveInterfaceProvider* Find(
        const semantic::MethodContract& canonicalOrigin) const;
};

struct InterfaceTablePlan final {
    InterfaceTableLayout layout;
    InterfaceProviderMap providerMap;
};

struct InterfaceLayoutAbi final {
    std::size_t pointerSize = sizeof(void*);
    std::size_t pointerAlignment = alignof(void*);
};

class InterfaceLoweringPlanner final {
   public:
    explicit InterfaceLoweringPlanner(InterfaceLayoutAbi abi = {});

    [[nodiscard]] InterfaceTableLayout BuildLayout(
        const semantic::InterfaceType& root) const;
    [[nodiscard]] InterfaceTablePlan BuildPlan(
        const semantic::StructureType& concrete,
        const semantic::InterfaceType& root) const;
    [[nodiscard]] InterfaceTablePlan BuildPlan(
        const semantic::EnumType& concrete,
        const semantic::InterfaceType& root) const;
    [[nodiscard]] std::vector<InterfaceTablePlan> BuildPlans(
        const semantic::SemanticModel& model) const;

   private:
    InterfaceLayoutAbi _abi;
};

}  // namespace flecha::lowering

#endif  // FLECHA_LOWERING_INTERFACE_LAYOUT_HPP
