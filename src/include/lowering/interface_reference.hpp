#ifndef FLECHA_LOWERING_INTERFACE_REFERENCE_HPP
#define FLECHA_LOWERING_INTERFACE_REFERENCE_HPP

#include <cstddef>

#include "semantic/semantic_type.hpp"

namespace flecha::lowering {

enum class InterfaceReferenceField : std::size_t {
    Data = 0,
    Table = 1,
};

inline constexpr std::size_t INTERFACE_REFERENCE_FIELD_COUNT = 2;

[[nodiscard]] inline bool IsInterfaceReference(semantic::SemanticType type) {
    return type.Kind() == semantic::SemanticTypeKind::Reference &&
           type.Target() != nullptr &&
           type.Target()->Kind() == semantic::SemanticTypeKind::Interface;
}

[[nodiscard]] inline constexpr std::size_t InterfaceReferenceFieldIndex(
    InterfaceReferenceField field) {
    return static_cast<std::size_t>(field);
}

}  // namespace flecha::lowering

#endif  // FLECHA_LOWERING_INTERFACE_REFERENCE_HPP
