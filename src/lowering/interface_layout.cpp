#include "lowering/interface_layout.hpp"

#include <algorithm>
#include <limits>
#include <optional>
#include <stdexcept>
#include <unordered_set>

namespace flecha::lowering {
namespace {

[[noreturn]] void InvariantFailure(const std::string& message) {
    throw std::logic_error("interface lowering invariant violated: " + message);
}

std::size_t AlignUp(std::size_t value, std::size_t alignment) {
    const std::size_t remainder = value % alignment;
    if (remainder == 0) return value;
    const std::size_t padding = alignment - remainder;
    if (value > std::numeric_limits<std::size_t>::max() - padding)
        InvariantFailure("table layout size overflow");
    return value + padding;
}

const semantic::MethodContract& CanonicalOrigin(
    const semantic::MethodContract& contract) {
    const semantic::MethodContract* origin = &contract;
    while (origin->inherited != nullptr) origin = origin->inherited;
    if (origin->declaredBy == nullptr)
        InvariantFailure("method lineage has no declaring interface");
    return *origin;
}

const semantic::MethodContract& EffectiveContract(
    const semantic::InterfaceType& root,
    const semantic::MethodContract& origin) {
    const semantic::MethodContract* result = nullptr;
    for (const semantic::MethodContract* candidate : root.methods) {
        if (candidate == nullptr)
            InvariantFailure("root contains a null method contract");
        if (candidate->key == origin.key) {
            if (result != nullptr)
                InvariantFailure("root contains duplicate effective method '" +
                                 origin.key.name + "'");
            result = candidate;
        }
    }
    if (result == nullptr)
        InvariantFailure("root is missing effective method '" +
                         origin.key.name + "'");
    if (&CanonicalOrigin(*result) != &origin)
        InvariantFailure("effective method has a different canonical origin");
    return *result;
}

InterfaceTableLayout BuildLayout(
    const semantic::InterfaceType& current, const semantic::InterfaceType& root,
    const InterfaceLayoutAbi& abi,
    const std::unordered_map<const semantic::MethodContract*,
                             std::vector<semantic::VariadicShape>>*
        variadicShapes,
    std::unordered_set<const semantic::InterfaceType*>& active,
    std::unordered_set<const semantic::InterfaceType*>& seen) {
    if (active.contains(&current))
        InvariantFailure("interface composition contains a cycle");
    if (!seen.insert(&current).second)
        InvariantFailure("interface occurs through more than one parent path");
    active.insert(&current);

    InterfaceTableLayout result;
    result.interfaceType = &current;
    std::size_t cursor = 0;

    result.parents.reserve(current.parents.size());
    for (const semantic::InterfaceReference& parent : current.parents) {
        if (parent.type == nullptr)
            InvariantFailure("interface has a null parent");
        InterfaceTableLayout parentTable =
            BuildLayout(*parent.type, root, abi, variadicShapes, active, seen);
        cursor = AlignUp(cursor, parentTable.alignment);
        result.alignment = std::max(result.alignment, parentTable.alignment);
        const std::size_t offset = cursor;
        if (cursor > std::numeric_limits<std::size_t>::max() - parentTable.size)
            InvariantFailure("table layout size overflow");
        cursor += parentTable.size;
        result.parents.push_back({parent.type, offset,
                                  std::make_shared<const InterfaceTableLayout>(
                                      std::move(parentTable))});
    }

    result.slots.reserve(current.declaredMethods.size());
    for (const semantic::MethodContract& declared : current.declaredMethods) {
        if (declared.declaredBy != &current)
            InvariantFailure("declared method belongs to another interface");
        if (declared.inherited != nullptr) continue;

        const semantic::MethodContract& origin = CanonicalOrigin(declared);
        const semantic::MethodContract& effective =
            EffectiveContract(root, origin);
        const auto appendSlot =
            [&](std::optional<semantic::VariadicShape> shape) {
                cursor = AlignUp(cursor, abi.pointerAlignment);
                result.alignment =
                    std::max(result.alignment, abi.pointerAlignment);
                result.slots.push_back(
                    {{&origin, &effective, std::move(shape)}, cursor});
                if (cursor >
                    std::numeric_limits<std::size_t>::max() - abi.pointerSize)
                    InvariantFailure("table layout size overflow");
                cursor += abi.pointerSize;
            };
        if (!origin.key.hasVariadicPack) {
            appendSlot(std::nullopt);
            continue;
        }
        if (variadicShapes == nullptr) continue;
        const auto shapes = variadicShapes->find(&origin);
        if (shapes == variadicShapes->end()) continue;
        for (const semantic::VariadicShape& shape : shapes->second)
            appendSlot(shape);
    }

    result.size = AlignUp(cursor, result.alignment);
    active.erase(&current);
    return result;
}

void FlattenSlots(const InterfaceTableLayout& layout,
                  std::vector<CanonicalInterfaceSlot>& result) {
    for (const ParentLayout& parent : layout.parents) {
        if (parent.table == nullptr)
            InvariantFailure("parent layout has no subtable");
        FlattenSlots(*parent.table, result);
    }
    for (const SlotLayout& slot : layout.slots) {
        if (slot.slot.origin == nullptr || slot.slot.effective == nullptr)
            InvariantFailure("layout contains an incomplete canonical slot");
        if (std::ranges::any_of(
                result, [&](const CanonicalInterfaceSlot& seen) {
                    return seen.origin == slot.slot.origin &&
                           (!seen.shape.has_value()
                                ? !slot.slot.shape.has_value()
                                : slot.slot.shape.has_value() &&
                                      seen.shape->key == slot.slot.shape->key);
                }))
            InvariantFailure("canonical slot occurs more than once");
        result.push_back(slot.slot);
    }
}

std::optional<std::size_t> InterfaceOffset(
    const InterfaceTableLayout& layout,
    const semantic::InterfaceType& interfaceType, std::size_t base = 0) {
    if (layout.interfaceType == &interfaceType) return base;
    for (const ParentLayout& parent : layout.parents) {
        const std::optional<std::size_t> found =
            InterfaceOffset(*parent.table, interfaceType, base + parent.offset);
        if (found.has_value()) return found;
    }
    return std::nullopt;
}

template <typename Concrete>
const semantic::FunctionSymbol* ConcreteProvider(
    const Concrete& concrete, const semantic::MethodContract& contract) {
    const std::string methodName =
        contract.key.name + (contract.isCrashable ? "!" : "");
    const auto found = concrete.methods.find(methodName);
    if (found == concrete.methods.end()) return nullptr;

    const semantic::FunctionSymbol* result = nullptr;
    for (const semantic::FunctionSymbol* method : found->second) {
        const semantic::MethodContract candidate = semantic::MethodContractOf(
            *method, method->IsProtected()
                         ? semantic::MethodVisibility::Protected
                         : semantic::MethodVisibility::Public);
        if (candidate.key != contract.key || method->IsPrivate() ||
            !candidate.SameContract(contract))
            continue;
        if (result != nullptr)
            InvariantFailure("concrete type has multiple providers for '" +
                             contract.key.name + "'");
        result = method;
    }
    return result;
}

template <typename Concrete>
InterfaceProviderMap BuildProviderMap(
    ConcreteInterfaceType concreteType, const Concrete& concrete,
    const semantic::InterfaceType& root, const InterfaceTableLayout& layout,
    const std::unordered_map<const semantic::FunctionSymbol*,
                             const semantic::FunctionSymbol*>* variadicTargets,
    const std::unordered_map<
        const semantic::FunctionSymbol*,
        std::unordered_map<std::string, const semantic::FunctionSymbol*>>*
        variadicShapeTargets) {
    InterfaceProviderMap result{concreteType, &root, {}};
    std::vector<CanonicalInterfaceSlot> slots;
    FlattenSlots(layout, slots);
    result.providers.reserve(slots.size());

    const auto dispatchTarget = [&](const semantic::FunctionSymbol* provider,
                                    const CanonicalInterfaceSlot& slot) {
        if (provider == nullptr) return provider;
        if (slot.shape.has_value()) {
            if (variadicShapeTargets == nullptr)
                InvariantFailure(
                    "variadic provider has no shape-target registry");
            const auto family = variadicShapeTargets->find(provider);
            if (family == variadicShapeTargets->end())
                InvariantFailure(
                    "variadic provider has no closed shape targets");
            const auto target = family->second.find(slot.shape->key);
            if (target == family->second.end())
                InvariantFailure(
                    "variadic provider has no target for closed shape");
            return target->second;
        }
        std::unordered_set<const semantic::FunctionSymbol*> seen;
        while (provider->Declaration() != nullptr &&
               provider->Declaration()->HasVariadicPack()) {
            if (!seen.insert(provider).second)
                InvariantFailure("variadic provider forwarding cycle");
            if (variadicTargets == nullptr)
                InvariantFailure("variadic provider has no target registry");
            const auto next = variadicTargets->find(provider);
            if (next == variadicTargets->end())
                InvariantFailure("variadic provider has no forwarding target");
            provider = next->second;
        }
        return provider;
    };

    for (const CanonicalInterfaceSlot& slot : slots) {
        const semantic::MethodContract& effective = *slot.effective;
        if (const semantic::FunctionSymbol* provider =
                ConcreteProvider(concrete, effective)) {
            result.providers.push_back(
                {slot, provider, dispatchTarget(provider, slot),
                 InterfaceProviderKind::ConcreteThunk, 0});
            continue;
        }

        if (effective.category == core::MethodCategory::Virtual)
            InvariantFailure("concrete type '" + concrete.displayName +
                             "' has unresolved virtual method '" +
                             effective.key.name + "'");
        if (effective.category != core::MethodCategory::Default &&
            effective.category != core::MethodCategory::Final)
            InvariantFailure("effective method has no provider category");
        if (effective.provider == nullptr || effective.declaredBy == nullptr)
            InvariantFailure("provided interface method '" +
                             effective.key.name + "' has no body");

        const std::optional<std::size_t> originOffset =
            InterfaceOffset(layout, *slot.origin->declaredBy);
        const std::optional<std::size_t> providerOffset =
            InterfaceOffset(layout, *effective.declaredBy);
        if (!originOffset.has_value() || !providerOffset.has_value())
            InvariantFailure("provider interface is outside the root table");
        const std::ptrdiff_t adjustment =
            static_cast<std::ptrdiff_t>(*providerOffset) -
            static_cast<std::ptrdiff_t>(*originOffset);
        result.providers.push_back(
            {slot, effective.provider, dispatchTarget(effective.provider, slot),
             adjustment == 0 ? InterfaceProviderKind::InterfaceBody
                             : InterfaceProviderKind::AdjustmentThunk,
             adjustment});
    }
    return result;
}

}  // namespace

std::string ConcreteInterfaceType::DisplayName() const {
    if (kind == ConcreteTypeKind::Structure && structure != nullptr)
        return structure->displayName;
    if (kind == ConcreteTypeKind::Enum && enumeration != nullptr)
        return enumeration->displayName;
    InvariantFailure("concrete type descriptor is incomplete");
}

const EffectiveInterfaceProvider* InterfaceProviderMap::Find(
    const semantic::MethodContract& canonicalOrigin,
    const semantic::VariadicShape* shape) const {
    const auto found = std::ranges::find_if(
        providers, [&](const EffectiveInterfaceProvider& provider) {
            if (provider.slot.origin != &canonicalOrigin) return false;
            if (shape == nullptr) return !provider.slot.shape.has_value();
            return provider.slot.shape.has_value() &&
                   provider.slot.shape->key == shape->key;
        });
    return found == providers.end() ? nullptr : &*found;
}

InterfaceLoweringPlanner::InterfaceLoweringPlanner(
    InterfaceLayoutAbi abi,
    const std::unordered_map<const semantic::MethodContract*,
                             std::vector<semantic::VariadicShape>>*
        variadicShapes,
    const std::unordered_map<const semantic::FunctionSymbol*,
                             const semantic::FunctionSymbol*>* variadicTargets,
    const std::unordered_map<
        const semantic::FunctionSymbol*,
        std::unordered_map<std::string, const semantic::FunctionSymbol*>>*
        variadicShapeTargets)
    : _abi(abi),
      _variadicShapes(variadicShapes),
      _variadicTargets(variadicTargets),
      _variadicShapeTargets(variadicShapeTargets) {
    if (_abi.pointerSize == 0 || _abi.pointerAlignment == 0)
        throw std::invalid_argument(
            "interface layout pointer size and alignment must be non-zero");
}

InterfaceTableLayout InterfaceLoweringPlanner::BuildLayout(
    const semantic::InterfaceType& root) const {
    std::unordered_set<const semantic::InterfaceType*> active;
    std::unordered_set<const semantic::InterfaceType*> seen;
    return lowering::BuildLayout(root, root, _abi, _variadicShapes, active,
                                 seen);
}

InterfaceTablePlan InterfaceLoweringPlanner::BuildPlan(
    const semantic::StructureType& concrete,
    const semantic::InterfaceType& root) const {
    InterfaceTableLayout layout = BuildLayout(root);
    InterfaceProviderMap providers = BuildProviderMap(
        {ConcreteTypeKind::Structure, &concrete, nullptr}, concrete, root,
        layout, _variadicTargets, _variadicShapeTargets);
    return {std::move(layout), std::move(providers)};
}

InterfaceTablePlan InterfaceLoweringPlanner::BuildPlan(
    const semantic::EnumType& concrete,
    const semantic::InterfaceType& root) const {
    InterfaceTableLayout layout = BuildLayout(root);
    InterfaceProviderMap providers =
        BuildProviderMap({ConcreteTypeKind::Enum, nullptr, &concrete}, concrete,
                         root, layout, _variadicTargets, _variadicShapeTargets);
    return {std::move(layout), std::move(providers)};
}

std::vector<InterfaceTablePlan> InterfaceLoweringPlanner::BuildPlans(
    const semantic::SemanticModel& model) const {
    std::vector<InterfaceTablePlan> result;
    for (const std::unique_ptr<semantic::StructureType>& concrete :
         model.Structures())
        for (const semantic::InterfaceReference& conformance :
             concrete->conformances)
            result.push_back(BuildPlan(*concrete, *conformance.type));
    for (const std::unique_ptr<semantic::EnumType>& concrete : model.Enums())
        for (const semantic::InterfaceReference& conformance :
             concrete->conformances)
            result.push_back(BuildPlan(*concrete, *conformance.type));
    return result;
}

}  // namespace flecha::lowering
