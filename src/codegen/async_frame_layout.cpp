#include "codegen/async_frame_layout.hpp"

#include <llvm/IR/DerivedTypes.h>

#include <algorithm>
#include <limits>
#include <unordered_map>

#include "runtime/async_executor.hpp"

namespace flecha::codegen {
namespace {

std::size_t Align(std::size_t value, std::size_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

class TypeMapper final {
   public:
    TypeMapper(llvm::LLVMContext& context, const llvm::DataLayout& layout)
        : _context(context), _layout(layout) {}

    llvm::Type* Map(semantic::SemanticType type) {
        using Kind = semantic::SemanticTypeKind;
        switch (type.Kind()) {
            case Kind::Enum:
                return Map(type.Enum()->backing);
            case Kind::Structure: {
                const auto found = _structures.find(type.Structure());
                if (found != _structures.end()) return found->second;
                auto* result = llvm::StructType::create(
                    _context, "flecha.async." + type.Structure()->name);
                _structures.emplace(type.Structure(), result);
                std::vector<llvm::Type*> fields;
                for (const auto& field : type.Structure()->fields) {
                    llvm::Type* mapped = Map(field.type);
                    if (!mapped) return nullptr;
                    fields.push_back(mapped);
                }
                result->setBody(fields);
                return result;
            }
            case Kind::ErrorValue:
                return llvm::StructType::get(
                    _context, {llvm::PointerType::getUnqual(_context),
                               llvm::Type::getInt32Ty(_context)});
            case Kind::NamedError:
                return llvm::Type::getInt32Ty(_context);
            case Kind::Tuple: {
                std::vector<llvm::Type*> fields;
                for (semantic::SemanticType element : type.Elements()) {
                    llvm::Type* mapped = Map(element);
                    if (!mapped) return nullptr;
                    fields.push_back(mapped);
                }
                return llvm::StructType::get(_context, fields);
            }
            case Kind::Slice:
            case Kind::OwnedBuffer:
                return llvm::StructType::get(
                    _context, {llvm::PointerType::getUnqual(_context),
                               llvm::IntegerType::get(
                                   _context, _layout.getPointerSizeInBits())});
            case Kind::OwnedSlice: {
                llvm::Type* size = llvm::IntegerType::get(
                    _context, _layout.getPointerSizeInBits());
                return llvm::StructType::get(
                    _context,
                    {llvm::PointerType::getUnqual(_context), size, size});
            }
            case Kind::Array: {
                llvm::Type* element = Map(*type.Target());
                return element ? llvm::ArrayType::get(element, type.Length())
                               : nullptr;
            }
            case Kind::I8:
            case Kind::U8:
            case Kind::Char:
                return llvm::Type::getInt8Ty(_context);
            case Kind::I16:
            case Kind::U16:
                return llvm::Type::getInt16Ty(_context);
            case Kind::I32:
            case Kind::U32:
                return llvm::Type::getInt32Ty(_context);
            case Kind::I64:
            case Kind::U64:
                return llvm::Type::getInt64Ty(_context);
            case Kind::ISize:
            case Kind::USize:
                return llvm::IntegerType::get(_context,
                                              _layout.getPointerSizeInBits());
            case Kind::F32:
                return llvm::Type::getFloatTy(_context);
            case Kind::F64:
                return llvm::Type::getDoubleTy(_context);
            case Kind::Bool:
                return llvm::Type::getInt1Ty(_context);
            case Kind::Reference:
            case Kind::Pointer:
            case Kind::Null:
                return llvm::PointerType::getUnqual(_context);
            case Kind::StringView: {
                llvm::Type* size = llvm::IntegerType::get(
                    _context, _layout.getPointerSizeInBits());
                return llvm::StructType::get(
                    _context, {llvm::PointerType::getUnqual(_context), size});
            }
            case Kind::String:
            case Kind::Function:
                return nullptr;
            case Kind::Void:
                return llvm::Type::getVoidTy(_context);
            default:
                return nullptr;
        }
    }

   private:
    llvm::LLVMContext& _context;
    const llvm::DataLayout& _layout;
    std::unordered_map<const semantic::StructureType*, llvm::StructType*>
        _structures;
};

}  // namespace

std::optional<AsyncConcreteFrameLayout> PlanAsyncFrameLayout(
    llvm::LLVMContext& context, const llvm::DataLayout& dataLayout,
    const semantic::AsyncFramePlan& plan, const lowering::AsyncMachine& machine,
    std::string& error) {
    error.clear();
    AsyncConcreteFrameLayout result;
    result.slotOffsets.resize(plan.slots.size());
    result.operationOffsets.resize(machine.OperationCount());
    TypeMapper mapper(context, dataLayout);
    std::size_t cursor = 0;
    auto reserve = [&](std::size_t size, std::size_t alignment) {
        cursor = Align(cursor, alignment);
        const std::size_t offset = cursor;
        cursor += size;
        result.alignment = std::max(result.alignment, alignment);
        return offset;
    };
    llvm::Type* pointer = llvm::PointerType::getUnqual(context);
    llvm::Type* control = llvm::StructType::get(
        context,
        {llvm::Type::getInt64Ty(context), llvm::Type::getInt8Ty(context),
         llvm::Type::getInt8Ty(context), pointer, pointer});
    result.controlOffset =
        reserve(dataLayout.getTypeAllocSize(control).getFixedValue(),
                dataLayout.getABITypeAlign(control).value());
    result.initializedOffset =
        reserve(std::max<std::size_t>(1, machine.SlotCount()), 1);
    result.activeOperationsOffset =
        reserve(std::max<std::size_t>(1, machine.OperationCount()), 1);
    result.resourceOffset =
        reserve(dataLayout.getPointerSize(),
                dataLayout.getPointerABIAlignment(0).value());

    for (std::size_t id = 0; id < plan.slots.size(); ++id) {
        const semantic::AsyncFrameSlot& slot = plan.slots[id];
        using Kind = semantic::AsyncSlotKind;
        if (slot.kind == Kind::Computation || slot.kind == Kind::ChildJoin ||
            slot.kind == Kind::TaskScope || slot.kind == Kind::DeferredCall ||
            slot.kind == Kind::DeferredStack)
            continue;
        llvm::Type* type = mapper.Map(slot.type);
        if (!type || type->isVoidTy() || !type->isSized()) {
            error = "unsupported concrete async slot type '" +
                    std::string(slot.type.Name()) + "'";
            return std::nullopt;
        }
        result.slotOffsets[id] =
            reserve(dataLayout.getTypeAllocSize(type).getFixedValue(),
                    dataLayout.getABITypeAlign(type).value());
    }
    for (std::size_t operation = 0; operation < machine.OperationCount();
         ++operation) {
        const std::size_t offset =
            reserve(sizeof(runtime::async::Computation),
                    alignof(runtime::async::Computation));
        result.operationOffsets[operation] = offset;
        result.slotOffsets[machine.OperationSlots()[operation]] = offset;
    }
    for (std::size_t state = 0; state < plan.states.size(); ++state) {
        const semantic::AsyncState& source = plan.states[state];
        if (source.kind != semantic::AsyncStateKind::DirectAwait ||
            !source.checked)
            continue;
        if (!source.checked->callee ||
            source.checked->arguments.size() !=
                source.checked->callee->ParameterTypes().size()) {
            error = "direct await has an invalid argument plan";
            return std::nullopt;
        }
        for (std::size_t argument = 0;
             argument < source.checked->arguments.size(); ++argument) {
            const semantic::SemanticType type =
                source.checked->arguments[argument]
                    ? source.checked->callee->ParameterTypes()[argument]
                    : semantic::SemanticType::Error();
            llvm::Type* mapped = mapper.Map(type);
            if (!mapped || !mapped->isSized()) {
                error = "unsupported async argument temporary type '" +
                        std::string(type.Name()) + "'";
                return std::nullopt;
            }
            const std::size_t alignment =
                dataLayout.getABITypeAlign(mapped).value();
            const std::size_t size =
                dataLayout.getTypeAllocSize(mapped).getFixedValue();
            result.temporaries.push_back({state, argument, type,
                                          reserve(size, alignment), size,
                                          alignment});
        }
    }
    if (cursor > std::numeric_limits<std::size_t>::max() - result.alignment) {
        error = "async frame layout size overflow";
        return std::nullopt;
    }
    result.size = Align(cursor, result.alignment);
    return result;
}

}  // namespace flecha::codegen
