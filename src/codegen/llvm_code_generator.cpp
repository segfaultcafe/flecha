#include "codegen/llvm_code_generator.hpp"

#include <llvm/ADT/APFloat.h>
#include <llvm/ADT/APInt.h>
#include <llvm/Config/llvm-config.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/TargetParser/Triple.h>

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>

#include "core/output_format.hpp"

namespace flecha::codegen {
namespace {

bool IsUnsigned(semantic::SemanticType type) {
    switch (type.Kind()) {
        case semantic::SemanticTypeKind::U8:
        case semantic::SemanticTypeKind::U16:
        case semantic::SemanticTypeKind::U32:
        case semantic::SemanticTypeKind::U64:
        case semantic::SemanticTypeKind::USize:
            return true;
        default:
            return false;
    }
}

llvm::Instruction* TerminatorOrNull(llvm::BasicBlock* block) {
#if LLVM_VERSION_MAJOR >= 23
    return block->getTerminatorOrNull();
#else
    return block->getTerminator();
#endif
}

bool CaptureStoredByValue(core::AnonymousFunctionExpression::CaptureMode mode) {
    using CaptureMode = core::AnonymousFunctionExpression::CaptureMode;
    return mode == CaptureMode::Copy || mode == CaptureMode::Lend ||
           mode == CaptureMode::Own;
}

}  // namespace

LlvmCodeGenerator::LlvmCodeGenerator() : _builder(_context) {}

void LlvmCodeGenerator::ConfigureTarget(std::string targetTriple,
                                        llvm::DataLayout dataLayout) {
    _targetTriple = std::move(targetTriple);
    _dataLayout = std::move(dataLayout);
}

bool LlvmCodeGenerator::Fail(std::string message) {
    if (_error.empty()) _error = std::move(message);
    return false;
}

llvm::Type* LlvmCodeGenerator::MapType(semantic::SemanticType type) {
    switch (type.Kind()) {
        case semantic::SemanticTypeKind::Enum:
            return MapType(type.Enum()->backing);
        case semantic::SemanticTypeKind::Structure: {
            const auto found = _structureTypes.find(type.Structure());
            if (found != _structureTypes.end()) return found->second;
            auto* result = llvm::StructType::create(
                _context, "flecha." + type.Structure()->name);
            _structureTypes[type.Structure()] = result;
            std::vector<llvm::Type*> fields;
            for (const auto& field : type.Structure()->fields)
                fields.push_back(MapType(field.type));
            result->setBody(fields);
            return result;
        }
        case semantic::SemanticTypeKind::ErrorValue:
            return llvm::StructType::get(
                _context, {llvm::PointerType::getUnqual(_context),
                           llvm::Type::getInt32Ty(_context)});
        case semantic::SemanticTypeKind::NamedError:
            return llvm::Type::getInt32Ty(_context);
        case semantic::SemanticTypeKind::Tuple: {
            std::vector<llvm::Type*> fields;
            for (const auto& element : type.Elements())
                fields.push_back(MapType(element));
            return llvm::StructType::get(_context, fields);
        }
        case semantic::SemanticTypeKind::Slice:
        case semantic::SemanticTypeKind::OwnedBuffer:
        case semantic::SemanticTypeKind::OwnedSlice:
            return llvm::StructType::get(
                _context, {llvm::PointerType::getUnqual(_context),
                           MapType(semantic::SemanticType(
                               semantic::SemanticTypeKind::USize))});
        case semantic::SemanticTypeKind::Array:
            return llvm::ArrayType::get(MapType(*type.Target()), type.Length());
        case semantic::SemanticTypeKind::I8:
        case semantic::SemanticTypeKind::U8:
        case semantic::SemanticTypeKind::Char:
            return llvm::Type::getInt8Ty(_context);
        case semantic::SemanticTypeKind::I16:
        case semantic::SemanticTypeKind::U16:
            return llvm::Type::getInt16Ty(_context);
        case semantic::SemanticTypeKind::I32:
        case semantic::SemanticTypeKind::U32:
            return llvm::Type::getInt32Ty(_context);
        case semantic::SemanticTypeKind::I64:
        case semantic::SemanticTypeKind::U64:
            return llvm::Type::getInt64Ty(_context);
        case semantic::SemanticTypeKind::ISize:
        case semantic::SemanticTypeKind::USize: {
            const unsigned width = _dataLayout.has_value()
                                       ? _dataLayout->getPointerSizeInBits()
                                       : 64;
            return llvm::IntegerType::get(_context, width);
        }
        case semantic::SemanticTypeKind::F32:
            return llvm::Type::getFloatTy(_context);
        case semantic::SemanticTypeKind::F64:
            return llvm::Type::getDoubleTy(_context);
        case semantic::SemanticTypeKind::Bool:
            return llvm::Type::getInt1Ty(_context);
        case semantic::SemanticTypeKind::Reference:
        case semantic::SemanticTypeKind::Pointer:
        case semantic::SemanticTypeKind::Null:
            return llvm::PointerType::getUnqual(_context);
        case semantic::SemanticTypeKind::Void:
            return llvm::Type::getVoidTy(_context);
        case semantic::SemanticTypeKind::StringView:
            return StringViewType();
        case semantic::SemanticTypeKind::String:
            return StringType();
        case semantic::SemanticTypeKind::Function:
            return llvm::StructType::get(
                _context, {llvm::PointerType::getUnqual(_context),
                           llvm::PointerType::getUnqual(_context),
                           llvm::PointerType::getUnqual(_context)});
        default:
            Fail("LLVM type is not implemented for '" +
                 std::string(type.Name()) + "'");
            return nullptr;
    }
}

llvm::StructType* LlvmCodeGenerator::StringViewType() {
    const unsigned width =
        _dataLayout.has_value() ? _dataLayout->getPointerSizeInBits() : 64;
    return llvm::StructType::get(_context,
                                 {llvm::PointerType::getUnqual(_context),
                                  llvm::IntegerType::get(_context, width)});
}

llvm::StructType* LlvmCodeGenerator::StringType() {
    const unsigned width =
        _dataLayout.has_value() ? _dataLayout->getPointerSizeInBits() : 64;
    llvm::Type* sizeType = llvm::IntegerType::get(_context, width);
    return llvm::StructType::get(
        _context, {llvm::PointerType::getUnqual(_context), sizeType, sizeType});
}

llvm::Value* LlvmCodeGenerator::EmitStringView(llvm::Value* value,
                                               semantic::SemanticType type) {
    if (type.Kind() == semantic::SemanticTypeKind::StringView) return value;
    if (type.Kind() != semantic::SemanticTypeKind::String) {
        Fail("cannot borrow type '" + std::string(type.Name()) +
             "' as stringv");
        return nullptr;
    }
    llvm::StructType* viewType = StringViewType();
    llvm::Value* view = llvm::UndefValue::get(viewType);
    view = _builder.CreateInsertValue(
        view, _builder.CreateExtractValue(value, 0, "string.data"), 0);
    return _builder.CreateInsertValue(
        view, _builder.CreateExtractValue(value, 1, "string.length"), 1,
        "string.view");
}

llvm::Value* LlvmCodeGenerator::EmitOwnedString(
    llvm::Value* value, semantic::SemanticType sourceType, bool temporary) {
    llvm::StructType* resultType = StringType();
    llvm::AllocaInst* output = nullptr;
    if (temporary) {
        output = CreateOwnedStringTemporary("string.result");
    } else {
        output = CreateEntryStorage(resultType, "string.transfer", true);
    }
    EmitStringStorageCleanup(output);
    const auto takeResult = [&](const char* name) -> llvm::Value* {
        llvm::Value* result = _builder.CreateLoad(resultType, output, name);
        if (temporary) {
            _freshOwnedStrings.emplace(result, output);
        } else {
            // Ownership moves to the caller. Re-executing this expression must
            // not destroy a pointer already held (or freed) by its destination.
            _builder.CreateStore(llvm::Constant::getNullValue(resultType),
                                 output);
        }
        return result;
    };
    if (sourceType.Kind() == semantic::SemanticTypeKind::String) {
        llvm::Value* data =
            _builder.CreateExtractValue(value, 0, "string.copy.data");
        llvm::Value* length =
            _builder.CreateExtractValue(value, 1, "string.copy.length");
        llvm::FunctionType* cloneType = llvm::FunctionType::get(
            llvm::Type::getVoidTy(_context),
            {output->getType(), data->getType(), length->getType()}, false);
        llvm::FunctionCallee clone = _module->getOrInsertFunction(
            "flecha_runtime_v1_string_clone", cloneType);
        _builder.CreateCall(clone, {output, data, length});
        return takeResult("string.copy");
    }
    if (sourceType.Kind() == semantic::SemanticTypeKind::StringView) {
        llvm::Value* data =
            _builder.CreateExtractValue(value, 0, "stringv.data");
        llvm::Value* length =
            _builder.CreateExtractValue(value, 1, "stringv.length");
        llvm::FunctionType* fromViewType = llvm::FunctionType::get(
            llvm::Type::getVoidTy(_context),
            {output->getType(), data->getType(), length->getType()}, false);
        llvm::FunctionCallee fromView = _module->getOrInsertFunction(
            "flecha_runtime_v1_string_from_view", fromViewType);
        _builder.CreateCall(fromView, {output, data, length});
        return takeResult("string");
    }

    std::string suffix;
    switch (sourceType.Kind()) {
        case semantic::SemanticTypeKind::I8:
            suffix = "i8";
            break;
        case semantic::SemanticTypeKind::I16:
            suffix = "i16";
            break;
        case semantic::SemanticTypeKind::I32:
            suffix = "i32";
            break;
        case semantic::SemanticTypeKind::I64:
            suffix = "i64";
            break;
        case semantic::SemanticTypeKind::ISize:
            suffix = "isize";
            break;
        case semantic::SemanticTypeKind::U8:
            suffix = "u8";
            break;
        case semantic::SemanticTypeKind::U16:
            suffix = "u16";
            break;
        case semantic::SemanticTypeKind::U32:
            suffix = "u32";
            break;
        case semantic::SemanticTypeKind::U64:
            suffix = "u64";
            break;
        case semantic::SemanticTypeKind::USize:
            suffix = "usize";
            break;
        case semantic::SemanticTypeKind::F32:
            suffix = "f32";
            break;
        case semantic::SemanticTypeKind::F64:
            suffix = "f64";
            break;
        case semantic::SemanticTypeKind::Bool:
            suffix = "bool";
            value = _builder.CreateZExt(value, llvm::Type::getInt8Ty(_context),
                                        "string.bool");
            break;
        case semantic::SemanticTypeKind::Char:
            suffix = "char";
            break;
        default:
            Fail("cannot convert type '" + std::string(sourceType.Name()) +
                 "' to string");
            return nullptr;
    }
    llvm::FunctionType* formatType =
        llvm::FunctionType::get(llvm::Type::getVoidTy(_context),
                                {output->getType(), value->getType()}, false);
    llvm::FunctionCallee format = _module->getOrInsertFunction(
        "flecha_runtime_v1_string_from_" + suffix, formatType);
    _builder.CreateCall(format, {output, value});
    return takeResult("string.format");
}

void LlvmCodeGenerator::EmitStringDestroy(llvm::Value* value) {
    llvm::Value* data =
        _builder.CreateExtractValue(value, 0, "string.destroy.data");
    llvm::FunctionType* destroyType = llvm::FunctionType::get(
        llvm::Type::getVoidTy(_context), {data->getType()}, false);
    llvm::FunctionCallee destroy = _module->getOrInsertFunction(
        "flecha_runtime_v1_string_destroy", destroyType);
    _builder.CreateCall(destroy, {data});
}

void LlvmCodeGenerator::EmitStringStorageCleanup(llvm::AllocaInst* storage) {
    EmitStringDestroy(
        _builder.CreateLoad(StringType(), storage, "string.cleanup"));
    _builder.CreateStore(llvm::Constant::getNullValue(StringType()), storage);
}

llvm::AllocaInst* LlvmCodeGenerator::CreateOwnedStringTemporary(
    std::string_view name) {
    llvm::StructType* type = StringType();
    llvm::AllocaInst* storage = CreateEntryStorage(type, name, true);
    _cleanupScopes.back()->ownedValues.push_back(
        {storage, semantic::SemanticType(semantic::SemanticTypeKind::String)});
    return storage;
}

llvm::Value* LlvmCodeGenerator::EmitValueCopy(llvm::Value* value,
                                              semantic::SemanticType type,
                                              bool temporary) {
    if (type.OwnsStorage() && !type.IsCopyable()) {
        const auto fresh = _freshOwnedValues.find(value);
        if (fresh != _freshOwnedValues.end()) {
            _builder.CreateStore(llvm::Constant::getNullValue(MapType(type)),
                                 fresh->second);
            _freshOwnedValues.erase(fresh);
            return value;
        }
    }
    if (type.Kind() == semantic::SemanticTypeKind::String && !temporary) {
        const auto fresh = _freshOwnedStrings.find(value);
        if (fresh != _freshOwnedStrings.end()) {
            // The temporary remains registered for lexical cleanup, so make
            // that cleanup inert after transferring its allocation.
            _builder.CreateStore(llvm::Constant::getNullValue(StringType()),
                                 fresh->second);
            _freshOwnedStrings.erase(fresh);
            return value;
        }
    }
    if (!type.IsCopyable()) return value;
    if (temporary && type.ContainsStructure()) {
        llvm::Value* copy = EmitValueCopy(value, type, false);
        if (!copy) return nullptr;
        auto* storage = CreateOwnedValueTemporary(type, "aggregate.copy");
        EmitValueStorageCleanup(storage, type);
        _builder.CreateStore(copy, storage);
        _builder.CreateStore(llvm::ConstantInt::getTrue(_context),
                             LifetimeFlag(storage));
        return _builder.CreateLoad(MapType(type), storage);
    }
    if (type.Kind() == semantic::SemanticTypeKind::String)
        return EmitOwnedString(value, type, temporary);
    if (type.Kind() == semantic::SemanticTypeKind::Structure) {
        llvm::Value* result = llvm::PoisonValue::get(MapType(type));
        for (std::size_t i = 0; i < type.Structure()->fields.size(); ++i) {
            llvm::Value* field =
                EmitValueCopy(_builder.CreateExtractValue(value, i),
                              type.Structure()->fields[i].type, temporary);
            if (!field) return nullptr;
            result = _builder.CreateInsertValue(result, field, i);
        }
        return result;
    }
    if (type.Kind() == semantic::SemanticTypeKind::Array &&
        type.OwnsStorage()) {
        auto* source =
            CreateTemporaryAlloca(MapType(type), "array.copy.source");
        auto* destination =
            CreateTemporaryAlloca(MapType(type), "array.copy.result");
        _builder.CreateStore(value, source);
        llvm::Value* index =
            llvm::ConstantInt::get(llvm::Type::getInt64Ty(_context), 0);
        // Aggregate copies require independent owning fields.
        auto* counter =
            CreateTemporaryAlloca(index->getType(), "array.copy.index");
        _builder.CreateStore(index, counter);
        EmitArrayElements(destination, type, false, [&](llvm::Value* slot) {
            llvm::Value* i = _builder.CreateLoad(index->getType(), counter);
            llvm::Value* from =
                _builder.CreateInBoundsGEP(MapType(type), source, {index, i});
            llvm::Value* item = EmitValueCopy(
                _builder.CreateLoad(MapType(*type.Target()), from),
                *type.Target(), temporary);
            if (item) _builder.CreateStore(item, slot);
            _builder.CreateStore(
                _builder.CreateAdd(i, llvm::ConstantInt::get(i->getType(), 1)),
                counter);
        });
        return _builder.CreateLoad(MapType(type), destination);
    }
    if (type.Kind() != semantic::SemanticTypeKind::Tuple || !type.OwnsStorage())
        return value;
    llvm::Value* result = llvm::PoisonValue::get(MapType(type));
    for (std::size_t i = 0; i < type.Elements().size(); ++i) {
        llvm::Value* field =
            EmitValueCopy(_builder.CreateExtractValue(value, i),
                          type.Elements()[i], temporary);
        if (field == nullptr) return nullptr;
        result = _builder.CreateInsertValue(result, field, i);
    }
    return result;
}

void LlvmCodeGenerator::EmitValueDestroy(llvm::Value* value,
                                         semantic::SemanticType type) {
    if (type.Kind() == semantic::SemanticTypeKind::Function) {
        llvm::Value* destroy =
            _builder.CreateExtractValue(value, 2, "function.destroy");
        llvm::Value* environment =
            _builder.CreateExtractValue(value, 1, "function.environment");
        llvm::Value* present = _builder.CreateICmpNE(
            destroy, llvm::Constant::getNullValue(destroy->getType()));
        auto* run = llvm::BasicBlock::Create(_context, "function.destroy.run",
                                             _currentFunction);
        auto* done = llvm::BasicBlock::Create(_context, "function.destroy.done",
                                              _currentFunction);
        _builder.CreateCondBr(present, run, done);
        _builder.SetInsertPoint(run);
        auto* type = llvm::FunctionType::get(llvm::Type::getVoidTy(_context),
                                             {environment->getType()}, false);
        _builder.CreateCall(type, destroy, {environment});
        _builder.CreateBr(done);
        _builder.SetInsertPoint(done);
        return;
    }
    if (type.ContainsStructure()) {
        auto* storage =
            CreateTemporaryAlloca(MapType(type), "aggregate.destroy");
        _builder.CreateStore(value, storage);
        EmitDestroyAt(storage, type);
        return;
    }
    if (type.Kind() == semantic::SemanticTypeKind::String)
        EmitStringDestroy(value);
    if (type.Kind() == semantic::SemanticTypeKind::OwnedSlice)
        EmitFree(value, type);
    if (type.Kind() == semantic::SemanticTypeKind::Array && type.OwnsStorage())
        for (std::size_t i = type.Length(); i > 0; --i)
            EmitValueDestroy(_builder.CreateExtractValue(value, i - 1),
                             *type.Target());
    if (type.Kind() == semantic::SemanticTypeKind::Tuple && type.OwnsStorage())
        for (std::size_t i = type.Elements().size(); i > 0; --i)
            EmitValueDestroy(_builder.CreateExtractValue(value, i - 1),
                             type.Elements()[i - 1]);
}

void LlvmCodeGenerator::EmitValueStorageCleanup(llvm::Value* storage,
                                                semantic::SemanticType type) {
    if (type.ContainsStructure()) {
        const auto flag = _lifetimeFlags.find(storage);
        if (flag != _lifetimeFlags.end()) {
            auto* destroy = llvm::BasicBlock::Create(
                _context, "structure.destroy", _currentFunction);
            auto* done = llvm::BasicBlock::Create(
                _context, "structure.destroy.done", _currentFunction);
            _builder.CreateCondBr(
                _builder.CreateLoad(llvm::Type::getInt1Ty(_context),
                                    flag->second),
                destroy, done);
            _builder.SetInsertPoint(destroy);
            _builder.CreateStore(llvm::ConstantInt::getFalse(_context),
                                 flag->second);
            EmitDestroyAt(storage, type);
            _builder.CreateBr(done);
            _builder.SetInsertPoint(done);
        } else
            EmitDestroyAt(storage, type);
        return;
    }
    if (type.Kind() == semantic::SemanticTypeKind::String) {
        EmitStringStorageCleanup(llvm::cast<llvm::AllocaInst>(storage));
        return;
    }
    EmitValueDestroy(_builder.CreateLoad(MapType(type), storage), type);
    _builder.CreateStore(llvm::Constant::getNullValue(MapType(type)), storage);
}

llvm::AllocaInst* LlvmCodeGenerator::CreateOwnedValueTemporary(
    semantic::SemanticType type, std::string_view name) {
    if (type.Kind() == semantic::SemanticTypeKind::String)
        return CreateOwnedStringTemporary(name);
    llvm::AllocaInst* storage = CreateEntryStorage(MapType(type), name, true);
    if (type.ContainsStructure()) LifetimeFlag(storage);
    _cleanupScopes.back()->ownedValues.push_back({storage, type});
    return storage;
}

bool LlvmCodeGenerator::EmitDestructure(
    const lowering::BoundDestructureStatement& statement) {
    const auto type = statement.Value().Type();
    llvm::Value* value = EmitExpression(statement.Value());
    if (value == nullptr) return false;
    // Capture every element before any destination is overwritten (including
    // swaps).
    value = EmitValueCopy(value, type, true);
    std::vector<llvm::Value*> values;
    for (std::size_t i = 0; i < statement.Bindings().size(); ++i) {
        const auto* variable = statement.Bindings()[i].variable;
        llvm::Value* field = nullptr;
        if (variable != nullptr) {
            field = CoerceValue(_builder.CreateExtractValue(value, i),
                                type.Elements()[i], variable->Type());
            if (field == nullptr) return false;
            if (type.IsCopyable())
                field = EmitValueCopy(field, variable->Type(), false);
        } else if (!type.IsCopyable() && type.Elements()[i].OwnsStorage()) {
            EmitValueDestroy(_builder.CreateExtractValue(value, i),
                             type.Elements()[i]);
        }
        values.push_back(field);
    }
    for (std::size_t i = 0; i < statement.Bindings().size(); ++i) {
        const auto& binding = statement.Bindings()[i];
        if (binding.variable == nullptr) continue;
        llvm::Value* storage = binding.declaration
                                   ? CreateEntryAlloca(*binding.variable)
                                   : _variables.at(binding.variable);
        const auto targetType = binding.variable->Type();
        if (targetType.ContainsStructure() && binding.declaration)
            LifetimeFlag(storage);
        if (targetType.OwnsStorage()) {
            EmitValueStorageCleanup(storage, targetType);
            if (binding.declaration)
                _cleanupScopes.back()->ownedValues.push_back(
                    {storage, targetType});
        }
        _builder.CreateStore(values[i], storage);
        if (targetType.ContainsStructure())
            _builder.CreateStore(llvm::ConstantInt::getTrue(_context),
                                 LifetimeFlag(storage));
        if (binding.declaration) _variables.emplace(binding.variable, storage);
    }
    return true;
}

void LlvmCodeGenerator::EmitCleanupsTo(std::size_t retainedScopeCount) {
    if (retainedScopeCount == _cleanupScopes.size()) return;
    // Defer emission until every slot in each lexical scope is registered.
    // Backward jumps may reach an earlier exit after later declarations (or
    // temporaries) have allocated strings. All slots start null in entry.
    llvm::BasicBlock* cleanup =
        llvm::BasicBlock::Create(_context, "cleanup", _currentFunction);
    _builder.CreateBr(cleanup);
    _builder.SetInsertPoint(cleanup);
    _cleanupRequests.push_back(
        {cleanup,
         {_cleanupScopes.begin() + retainedScopeCount, _cleanupScopes.end()}});
}

void LlvmCodeGenerator::InitializeDeferredScope(const CleanupScope& scope) {
    if (scope.deferredHead == nullptr) return;
    llvm::Type* pointer = llvm::PointerType::getUnqual(_context);
    _builder.CreateStore(llvm::Constant::getNullValue(pointer),
                         scope.deferredHead);
#if LLVM_VERSION_MAJOR >= 20
    llvm::Function* save = llvm::Intrinsic::getOrInsertDeclaration(
        _module.get(), llvm::Intrinsic::stacksave, {pointer});
#elif LLVM_VERSION_MAJOR >= 18
    llvm::Function* save = llvm::Intrinsic::getDeclaration(
        _module.get(), llvm::Intrinsic::stacksave, {pointer});
#else
    llvm::Function* save = llvm::Intrinsic::getDeclaration(
        _module.get(), llvm::Intrinsic::stacksave);
#endif
    _builder.CreateStore(_builder.CreateCall(save), scope.stackMark);
}

bool LlvmCodeGenerator::EmitDefer(
    const lowering::BoundDeferStatement& statement) {
    const CleanupScope& scope = *_cleanupScopes.back();
    std::size_t site = 0;
    while (scope.deferredCalls[site].statement != &statement) ++site;
    const DeferredCall& deferred = scope.deferredCalls[site];
    const auto& call = statement.Call();
    std::vector<const lowering::BoundExpression*> captures;
    if (call.Kind() == lowering::BoundNodeKind::CallExpression) {
        const auto& direct =
            static_cast<const lowering::BoundCallExpression&>(call);
        for (const auto& argument : direct.Arguments())
            captures.push_back(argument.get());
    } else if (call.Kind() == lowering::BoundNodeKind::IndirectCallExpression) {
        const auto& indirect =
            static_cast<const lowering::BoundIndirectCallExpression&>(call);
        captures.push_back(&indirect.Callee());
        for (const auto& argument : indirect.Arguments())
            captures.push_back(argument.get());
    } else {
        return Fail("unsupported deferred call");
    }
    std::vector<llvm::Value*> captured;
    for (std::size_t index = 0; index < captures.size(); ++index) {
        const auto& argument = *captures[index];
        llvm::Value* value = EmitExpression(argument);
        if (value == nullptr) return false;
        const auto type = deferred.captureTypes[index];
        // Views also receive an owned snapshot, so later mutation/destruction
        // of the original string cannot invalidate a pending deferred call.
        if (type.Kind() == semantic::SemanticTypeKind::String ||
            type.Kind() == semantic::SemanticTypeKind::StringView)
            value = EmitOwnedString(value, argument.Type(), false);
        else
            value = CoerceValue(value, argument.Type(), type);
        if (value == nullptr) return false;
        if ((type.Kind() == semantic::SemanticTypeKind::Tuple ||
             type.ContainsStructure()) &&
            !deferred.captureOwnershipTransfers[index])
            value = EmitValueCopy(value, type, false);
        captured.push_back(value);
    }
    // This alloca is deliberately at registration, not in the entry block:
    // backward jumps preserve every instance, each with its own argument
    // values.
    llvm::AllocaInst* record =
        _builder.CreateAlloca(deferred.recordType, nullptr, "defer.record");
    llvm::Type* pointer = llvm::PointerType::getUnqual(_context);
    _builder.CreateStore(
        _builder.CreateLoad(pointer, scope.deferredHead),
        _builder.CreateStructGEP(deferred.recordType, record, 0));
    _builder.CreateStore(
        llvm::ConstantInt::get(llvm::Type::getInt32Ty(_context), site),
        _builder.CreateStructGEP(deferred.recordType, record, 1));
    for (std::size_t index = 0; index < captured.size(); ++index)
        _builder.CreateStore(
            captured[index],
            _builder.CreateStructGEP(deferred.recordType, record, index + 2));
    _builder.CreateStore(record, scope.deferredHead);
    return true;
}

bool LlvmCodeGenerator::EmitDeferredCalls(const CleanupScope& scope) {
    if (scope.deferredHead == nullptr) return true;
    llvm::Type* pointer = llvm::PointerType::getUnqual(_context);
    llvm::Type* tagType = llvm::Type::getInt32Ty(_context);
    llvm::StructType* header =
        llvm::StructType::get(_context, {pointer, tagType});
    llvm::BasicBlock* check =
        llvm::BasicBlock::Create(_context, "defer.check", _currentFunction);
    llvm::BasicBlock* dispatch =
        llvm::BasicBlock::Create(_context, "defer.dispatch", _currentFunction);
    llvm::BasicBlock* done =
        llvm::BasicBlock::Create(_context, "defer.done", _currentFunction);
    llvm::BasicBlock* invalid =
        llvm::BasicBlock::Create(_context, "defer.invalid", _currentFunction);
    _builder.CreateBr(check);
    _builder.SetInsertPoint(check);
    llvm::Value* record =
        _builder.CreateLoad(pointer, scope.deferredHead, "defer.pending");
    _builder.CreateCondBr(
        _builder.CreateICmpNE(record, llvm::Constant::getNullValue(pointer)),
        dispatch, done);
    _builder.SetInsertPoint(dispatch);
    llvm::Value* previous = _builder.CreateLoad(
        pointer, _builder.CreateStructGEP(header, record, 0));
    llvm::Value* tag = _builder.CreateLoad(
        tagType, _builder.CreateStructGEP(header, record, 1));
    _builder.CreateStore(previous, scope.deferredHead);
    llvm::SwitchInst* select =
        _builder.CreateSwitch(tag, invalid, scope.deferredCalls.size());
    for (std::size_t site = 0; site < scope.deferredCalls.size(); ++site) {
        const DeferredCall& deferred = scope.deferredCalls[site];
        const auto& call = deferred.statement->Call();
        llvm::BasicBlock* invoke = llvm::BasicBlock::Create(
            _context, "defer.invoke", _currentFunction);
        select->addCase(
            llvm::ConstantInt::get(llvm::Type::getInt32Ty(_context), site),
            invoke);
        _builder.SetInsertPoint(invoke);
        std::vector<llvm::Value*> captured, strings;
        llvm::Value* retainedCallable = nullptr;
        for (std::size_t index = 0; index < deferred.captureTypes.size();
             ++index) {
            const auto type = deferred.captureTypes[index];
            const bool string =
                type.Kind() == semantic::SemanticTypeKind::String ||
                type.Kind() == semantic::SemanticTypeKind::StringView;
            llvm::Value* value =
                _builder.CreateLoad(string ? StringType() : MapType(type),
                                    _builder.CreateStructGEP(
                                        deferred.recordType, record, index + 2),
                                    "defer.argument");
            if (string) {
                strings.push_back(value);
                if (type.Kind() == semantic::SemanticTypeKind::StringView)
                    value = EmitStringView(
                        value, semantic::SemanticType(
                                   semantic::SemanticTypeKind::String));
            }
            captured.push_back(value);
        }
        if (deferred.indirect) {
            const auto& indirect =
                static_cast<const lowering::BoundIndirectCallExpression&>(call);
            const auto& signature = indirect.Callee().Type().Signature();
            llvm::Value* callable = captured.front();
            retainedCallable = callable;
            llvm::Value* target =
                _builder.CreateExtractValue(callable, 0, "function.invoke");
            llvm::Value* environment = _builder.CreateExtractValue(
                callable, 1, "function.environment");
            std::vector<llvm::Value*> arguments;
            llvm::AllocaInst* resultStorage = nullptr;
            if (signature.result.ContainsStructure()) {
                resultStorage = CreateTemporaryAlloca(MapType(signature.result),
                                                      "defer.result");
                arguments.push_back(resultStorage);
            }
            arguments.push_back(environment);
            arguments.insert(arguments.end(), captured.begin() + 1,
                             captured.end());
            llvm::Value* result = _builder.CreateCall(
                MapCallableInvokeType(signature), target, arguments,
                signature.result.Kind() == semantic::SemanticTypeKind::Void ||
                        signature.result.ContainsStructure()
                    ? ""
                    : "defer.indirect.result");
            if (resultStorage != nullptr)
                EmitDestroyAt(resultStorage, signature.result);
            else
                EmitValueDestroy(result, signature.result);
        } else {
            const auto& direct =
                static_cast<const lowering::BoundCallExpression&>(call);
            if (direct.Function().BuiltIn() ==
                semantic::BuiltInFunction::Free) {
                EmitFree(captured.front(),
                         direct.Function().ParameterTypes()[0]);
            } else if (direct.Function().BuiltIn() !=
                       semantic::BuiltInFunction::None) {
                if (EmitBuiltInOutput(direct, captured) == nullptr)
                    return false;
            } else if (direct.Type().ContainsStructure()) {
                // Construct discarded results in their final storage too:
                // an unsafe tear may rely on the object's address.
                llvm::AllocaInst* result = CreateTemporaryAlloca(
                    MapType(direct.Type()), "defer.result");
                std::vector<llvm::Value*> arguments{result};
                arguments.insert(arguments.end(), captured.begin(),
                                 captured.end());
                EmitFunctionCall(direct.Function(), arguments);
                EmitDestroyAt(result, direct.Type());
            } else {
                llvm::Value* result =
                    EmitFunctionCall(direct.Function(), captured);
                EmitValueDestroy(result, direct.Type());
            }
        }
        const std::size_t firstArgument = deferred.indirect ? 1 : 0;
        for (std::size_t i = captured.size(); i > firstArgument; --i)
            if ((deferred.captureTypes[i - 1].Kind() ==
                     semantic::SemanticTypeKind::Tuple ||
                 deferred.captureTypes[i - 1].ContainsStructure()) &&
                !deferred.captureOwnershipTransfers[i - 1])
                EmitValueDestroy(captured[i - 1], deferred.captureTypes[i - 1]);
        for (auto string = strings.rbegin(); string != strings.rend(); ++string)
            EmitStringDestroy(*string);
        if (retainedCallable != nullptr)
            EmitValueDestroy(retainedCallable, deferred.captureTypes.front());
        _builder.CreateBr(check);
    }
    _builder.SetInsertPoint(invalid);
    _builder.CreateUnreachable();
    _builder.SetInsertPoint(done);
#if LLVM_VERSION_MAJOR >= 20
    llvm::Function* restore = llvm::Intrinsic::getOrInsertDeclaration(
        _module.get(), llvm::Intrinsic::stackrestore, {pointer});
#elif LLVM_VERSION_MAJOR >= 18
    llvm::Function* restore = llvm::Intrinsic::getDeclaration(
        _module.get(), llvm::Intrinsic::stackrestore, {pointer});
#else
    llvm::Function* restore = llvm::Intrinsic::getDeclaration(
        _module.get(), llvm::Intrinsic::stackrestore);
#endif
    _builder.CreateCall(restore,
                        {_builder.CreateLoad(pointer, scope.stackMark)});
    return true;
}

llvm::Value* LlvmCodeGenerator::CoerceValue(
    llvm::Value* value, semantic::SemanticType sourceType,
    semantic::SemanticType destinationType) {
    if (sourceType.Kind() == semantic::SemanticTypeKind::NamedError &&
        destinationType.Kind() == semantic::SemanticTypeKind::ErrorValue) {
        const std::string name = ".flecha.error." + sourceType.Family().name;
        llvm::GlobalVariable* identity = _module->getNamedGlobal(name);
        if (identity == nullptr) {
            // The address is the family identity: do not mark it unnamed_addr.
            identity = new llvm::GlobalVariable(
                *_module, llvm::Type::getInt8Ty(_context), true,
                llvm::GlobalValue::PrivateLinkage,
                llvm::ConstantInt::get(llvm::Type::getInt8Ty(_context), 0),
                name);
        }
        llvm::Value* zero = llvm::ConstantInt::get(value->getType(), 0);
        llvm::Value* family =
            _builder.CreateSelect(_builder.CreateICmpEQ(value, zero),
                                  llvm::ConstantPointerNull::get(
                                      llvm::PointerType::getUnqual(_context)),
                                  identity, "error.family");
        llvm::Value* result = llvm::PoisonValue::get(MapType(destinationType));
        result = _builder.CreateInsertValue(result, family, 0);
        return _builder.CreateInsertValue(result, value, 1, "error.value");
    }
    if (sourceType.Kind() == semantic::SemanticTypeKind::Null &&
        destinationType.Kind() == semantic::SemanticTypeKind::Pointer)
        return value;
    if (sourceType.Kind() == semantic::SemanticTypeKind::Null &&
        (destinationType.Kind() == semantic::SemanticTypeKind::OwnedBuffer ||
         destinationType.Kind() == semantic::SemanticTypeKind::OwnedSlice))
        return llvm::Constant::getNullValue(MapType(destinationType));
    if (sourceType.Kind() == semantic::SemanticTypeKind::OwnedBuffer &&
        destinationType.Kind() == semantic::SemanticTypeKind::Pointer &&
        semantic::IsAssignable(destinationType, sourceType))
        return _builder.CreateExtractValue(value, 0, "owner.data");
    if (sourceType.Kind() == semantic::SemanticTypeKind::Pointer &&
        destinationType.Kind() == semantic::SemanticTypeKind::Pointer &&
        semantic::IsExternAssignable(destinationType, sourceType))
        return value;
    if (sourceType.Kind() == semantic::SemanticTypeKind::Pointer &&
        destinationType.Kind() == semantic::SemanticTypeKind::Reference &&
        sourceType.Target()->Kind() == semantic::SemanticTypeKind::Structure &&
        sourceType.Target()->WithoutConst() ==
            destinationType.Target()->WithoutConst())
        return value;
    if (semantic::CanReborrow(destinationType, sourceType)) return value;
    if (sourceType.Kind() == semantic::SemanticTypeKind::Function &&
        destinationType.Kind() == semantic::SemanticTypeKind::Function &&
        semantic::IsAssignable(destinationType, sourceType))
        return value;
    if (sourceType.WithoutConst() == destinationType.WithoutConst())
        return value;
    if (sourceType.Kind() == semantic::SemanticTypeKind::Char &&
        destinationType.Kind() == semantic::SemanticTypeKind::U8)
        return value;
    if (sourceType.Kind() == semantic::SemanticTypeKind::Tuple &&
        destinationType.Kind() == semantic::SemanticTypeKind::Tuple) {
        llvm::Value* result = llvm::PoisonValue::get(MapType(destinationType));
        for (std::size_t i = 0; i < sourceType.Elements().size(); ++i) {
            llvm::Value* field = CoerceValue(
                _builder.CreateExtractValue(value, i), sourceType.Elements()[i],
                destinationType.Elements()[i]);
            if (field == nullptr) return nullptr;
            result = _builder.CreateInsertValue(result, field, i);
        }
        return result;
    }
    if (destinationType.Kind() == semantic::SemanticTypeKind::String) {
        return EmitOwnedString(value, sourceType);
    }
    if (destinationType.Kind() == semantic::SemanticTypeKind::StringView &&
        sourceType.Kind() == semantic::SemanticTypeKind::String) {
        return EmitStringView(value, sourceType);
    }
    Fail("LLVM coercion is not implemented from '" +
         std::string(sourceType.Name()) + "' to '" +
         std::string(destinationType.Name()) + "'");
    return nullptr;
}

llvm::Value* LlvmCodeGenerator::EmitRuntimeOutput(llvm::Value* value,
                                                  semantic::SemanticType type,
                                                  bool newline) {
    std::string suffix;
    std::vector<llvm::Value*> arguments;
    std::vector<llvm::Type*> parameterTypes;
    if (type.Kind() == semantic::SemanticTypeKind::StringView ||
        type.Kind() == semantic::SemanticTypeKind::String) {
        suffix = "string_view";
        llvm::Value* data =
            _builder.CreateExtractValue(value, 0, "stdout.data");
        llvm::Value* length =
            _builder.CreateExtractValue(value, 1, "stdout.length");
        arguments = {data, length};
        parameterTypes = {data->getType(), length->getType()};
    } else {
        switch (type.Kind()) {
            case semantic::SemanticTypeKind::I8:
                suffix = "i8";
                break;
            case semantic::SemanticTypeKind::I16:
                suffix = "i16";
                break;
            case semantic::SemanticTypeKind::I32:
                suffix = "i32";
                break;
            case semantic::SemanticTypeKind::I64:
                suffix = "i64";
                break;
            case semantic::SemanticTypeKind::ISize:
                suffix = "isize";
                break;
            case semantic::SemanticTypeKind::U8:
                suffix = "u8";
                break;
            case semantic::SemanticTypeKind::U16:
                suffix = "u16";
                break;
            case semantic::SemanticTypeKind::U32:
                suffix = "u32";
                break;
            case semantic::SemanticTypeKind::U64:
                suffix = "u64";
                break;
            case semantic::SemanticTypeKind::USize:
                suffix = "usize";
                break;
            case semantic::SemanticTypeKind::F32:
                suffix = "f32";
                break;
            case semantic::SemanticTypeKind::F64:
                suffix = "f64";
                break;
            case semantic::SemanticTypeKind::Bool:
                suffix = "bool";
                value = _builder.CreateZExt(
                    value, llvm::Type::getInt8Ty(_context), "stdout.bool");
                break;
            case semantic::SemanticTypeKind::Char:
                suffix = "char";
                break;
            default:
                Fail("stdout does not support type '" +
                     std::string(type.Name()) + "'");
                return nullptr;
        }
        arguments = {value};
        parameterTypes = {value->getType()};
    }

    llvm::FunctionType* outputType = llvm::FunctionType::get(
        llvm::Type::getVoidTy(_context), parameterTypes, false);
    llvm::FunctionCallee output = _module->getOrInsertFunction(
        "flecha_runtime_v1_stdout_" + suffix, outputType);
    llvm::Value* call = _builder.CreateCall(output, arguments);
    if (!newline) return call;

    return EmitRuntimeNewline();
}

llvm::Value* LlvmCodeGenerator::EmitRuntimeNewline() {
    llvm::FunctionType* newlineType =
        llvm::FunctionType::get(llvm::Type::getVoidTy(_context), {}, false);
    llvm::FunctionCallee emitNewline = _module->getOrInsertFunction(
        "flecha_runtime_v1_stdout_newline", newlineType);
    return _builder.CreateCall(emitNewline);
}

llvm::Value* LlvmCodeGenerator::EmitBuiltInOutput(
    const lowering::BoundCallExpression& expression,
    const std::vector<llvm::Value*>& arguments) {
    const semantic::BuiltInFunction builtIn = expression.Function().BuiltIn();
    llvm::Value* result = nullptr;
    if (builtIn == semantic::BuiltInFunction::Stdout ||
        builtIn == semantic::BuiltInFunction::StdoutLine) {
        for (std::size_t index = 0; index < arguments.size(); ++index) {
            result = EmitRuntimeOutput(
                arguments[index], expression.Arguments()[index]->Type(), false);
            if (result == nullptr) return nullptr;
        }
        if (builtIn == semantic::BuiltInFunction::StdoutLine)
            return EmitRuntimeNewline();
    } else if (builtIn == semantic::BuiltInFunction::StdoutFormat) {
        if (expression.Arguments().empty() ||
            expression.Arguments().front()->Kind() !=
                lowering::BoundNodeKind::LiteralExpression)
            return Fail("stdoutf format was not lowered as a literal"), nullptr;
        const auto& literal =
            static_cast<const lowering::BoundLiteralExpression&>(
                *expression.Arguments().front());
        const core::OutputFormat format =
            core::ParseOutputFormat(literal.Value());
        if (!format.IsValid())
            return Fail("invalid stdoutf format reached code generation"),
                   nullptr;

        std::size_t argumentIndex = 1;
        for (const auto& part : format.parts) {
            if (part.kind == core::OutputFormatPartKind::Literal) {
                const lowering::BoundLiteralExpression text(
                    expression.Range(),
                    semantic::SemanticType(
                        semantic::SemanticTypeKind::StringView),
                    part.text);
                llvm::Value* value = EmitLiteral(text);
                if (value == nullptr) return nullptr;
                result = EmitRuntimeOutput(
                    value,
                    semantic::SemanticType(
                        semantic::SemanticTypeKind::StringView),
                    false);
                if (result == nullptr) return nullptr;
                continue;
            }
            if (argumentIndex >= arguments.size())
                return Fail("stdoutf argument count changed after analysis"),
                       nullptr;
            semantic::SemanticType type =
                expression.Arguments()[argumentIndex]->Type();
            llvm::Value* value = arguments[argumentIndex];
            if (part.kind == core::OutputFormatPartKind::Boolean &&
                type.Kind() == semantic::SemanticTypeKind::U8) {
                value = _builder.CreateICmpNE(
                    value, llvm::ConstantInt::get(value->getType(), 0),
                    "stdoutf.byte.bool");
                type = semantic::SemanticType(semantic::SemanticTypeKind::Bool);
            }
            result = EmitRuntimeOutput(value, type, false);
            if (result == nullptr) return nullptr;
            ++argumentIndex;
        }
    } else {
        return Fail("unsupported output built-in"), nullptr;
    }

    if (result != nullptr) return result;
    const lowering::BoundLiteralExpression empty(
        expression.Range(),
        semantic::SemanticType(semantic::SemanticTypeKind::StringView), "");
    llvm::Value* value = EmitLiteral(empty);
    return value == nullptr
               ? nullptr
               : EmitRuntimeOutput(value,
                                   semantic::SemanticType(
                                       semantic::SemanticTypeKind::StringView),
                                   false);
}

llvm::Value* LlvmCodeGenerator::EmitStringViewOperation(
    const lowering::BoundStringViewOperationExpression& expression) {
    llvm::Value* receiver = EmitExpression(expression.Receiver());
    if (receiver == nullptr) return nullptr;
    if (expression.Operation() == lowering::StringViewOperation::ToString) {
        return EmitOwnedString(receiver, expression.Receiver().Type());
    }
    if (expression.Operation() == lowering::StringViewOperation::Copy) {
        return EmitOwnedString(receiver, expression.Receiver().Type());
    }
    if (expression.Operation() == lowering::StringViewOperation::View) {
        return EmitStringView(receiver, expression.Receiver().Type());
    }
    llvm::Value* view = EmitStringView(receiver, expression.Receiver().Type());
    if (view == nullptr) return nullptr;
    llvm::Value* data = _builder.CreateExtractValue(view, 0, "stringv.data");
    llvm::Value* length =
        _builder.CreateExtractValue(view, 1, "stringv.length");

    if (expression.Operation() == lowering::StringViewOperation::Data)
        return data;

    if (expression.Operation() == lowering::StringViewOperation::Length) {
        return _builder.CreateZExtOrTrunc(
            length, llvm::Type::getInt32Ty(_context), "stringv.len");
    }

    std::vector<llvm::Value*> arguments;
    for (const std::unique_ptr<lowering::BoundExpression>& argument :
         expression.Arguments()) {
        llvm::Value* value = EmitExpression(*argument);
        if (value == nullptr) return nullptr;
        arguments.push_back(_builder.CreateZExtOrTrunc(
            value, llvm::Type::getInt32Ty(_context), "stringv.index"));
    }

    if (expression.Operation() == lowering::StringViewOperation::At) {
        llvm::FunctionType* atType =
            llvm::FunctionType::get(llvm::Type::getInt8Ty(_context),
                                    {data->getType(), length->getType(),
                                     llvm::Type::getInt32Ty(_context)},
                                    false);
        llvm::FunctionCallee at = _module->getOrInsertFunction(
            "flecha_runtime_v1_string_view_at", atType);
        return _builder.CreateCall(at, {data, length, arguments.front()},
                                   "stringv.at");
    }

    llvm::FunctionType* checkType = llvm::FunctionType::get(
        llvm::Type::getVoidTy(_context),
        {length->getType(), llvm::Type::getInt32Ty(_context),
         llvm::Type::getInt32Ty(_context)},
        false);
    llvm::FunctionCallee check = _module->getOrInsertFunction(
        "flecha_runtime_v1_string_view_check_substr", checkType);
    _builder.CreateCall(check, {length, arguments[0], arguments[1]});
    llvm::Value* start = _builder.CreateZExt(arguments[0], length->getType(),
                                             "stringv.substr.start");
    llvm::Value* substringData = _builder.CreateInBoundsGEP(
        llvm::Type::getInt8Ty(_context), data, start, "stringv.substr.data");
    llvm::Value* substringLength = _builder.CreateZExt(
        arguments[1], length->getType(), "stringv.substr.length");
    llvm::StructType* type = StringViewType();
    llvm::Value* substring = llvm::UndefValue::get(type);
    substring = _builder.CreateInsertValue(substring, substringData, 0);
    return _builder.CreateInsertValue(substring, substringLength, 1,
                                      "stringv.substr");
}

llvm::FunctionType* LlvmCodeGenerator::MapFunctionType(
    const lowering::BoundFunction& boundFunction) {
    const semantic::FunctionSymbol& function = boundFunction.Symbol();
    llvm::Type* returnType = function.ReturnType().ContainsStructure()
                                 ? llvm::Type::getVoidTy(_context)
                                 : MapType(function.ReturnType());
    if (returnType == nullptr) return nullptr;
    std::vector<llvm::Type*> parameters;
    parameters.reserve(function.ParameterTypes().size() + 1);
    if (function.ReturnType().ContainsStructure())
        parameters.push_back(llvm::PointerType::getUnqual(_context));
    if (!boundFunction.Captures().empty())
        parameters.push_back(llvm::PointerType::getUnqual(_context));
    for (semantic::SemanticType parameter : function.ParameterTypes()) {
        llvm::Type* parameterType = MapType(parameter);
        if (parameterType == nullptr) return nullptr;
        parameters.push_back(parameterType);
    }
    return llvm::FunctionType::get(returnType, parameters, false);
}

llvm::FunctionType* LlvmCodeGenerator::MapCallableInvokeType(
    const semantic::CallableSignature& signature) {
    llvm::Type* returnType = signature.result.ContainsStructure()
                                 ? llvm::Type::getVoidTy(_context)
                                 : MapType(signature.result);
    if (returnType == nullptr) return nullptr;
    std::vector<llvm::Type*> parameters;
    parameters.reserve(signature.parameters.size() + 2);
    if (signature.result.ContainsStructure())
        parameters.push_back(llvm::PointerType::getUnqual(_context));
    parameters.push_back(llvm::PointerType::getUnqual(_context));
    for (const auto& parameter : signature.parameters) {
        auto* type = MapType(parameter);
        if (type == nullptr) return nullptr;
        parameters.push_back(type);
    }
    return llvm::FunctionType::get(returnType, parameters, false);
}

std::optional<llvm::Attribute::AttrKind>
LlvmCodeGenerator::ExternIntegerExtension(semantic::SemanticType type) const {
    using Kind = semantic::SemanticTypeKind;
    const llvm::Triple triple(_targetTriple);

    // AArch64's base PCS leaves sub-32-bit integer extension to the callee,
    // except for Darwin's C ABI. The Windows x64 ABI does the same for C
    // integer types, while still requiring zero extension for _Bool.
    if (triple.isAArch64() && !triple.isOSDarwin()) return std::nullopt;
    if (triple.getArch() == llvm::Triple::x86_64 && triple.isOSWindows() &&
        type.Kind() != Kind::Bool)
        return std::nullopt;

    switch (type.Kind()) {
        case Kind::I8:
        case Kind::I16:
            return llvm::Attribute::SExt;
        case Kind::U8:
        case Kind::U16:
        case Kind::Char:
        case Kind::Bool:
            return llvm::Attribute::ZExt;
        default:
            return std::nullopt;
    }
}

void LlvmCodeGenerator::ApplyExternAttributes(
    const semantic::FunctionSymbol& symbol, llvm::Function& function) {
    if (!symbol.IsExtern()) return;
    if (const auto attribute = ExternIntegerExtension(symbol.ReturnType()))
        function.addRetAttr(*attribute);
    for (std::size_t index = 0; index < symbol.ParameterTypes().size(); ++index)
        if (const auto attribute =
                ExternIntegerExtension(symbol.ParameterTypes()[index]))
            function.addParamAttr(index, *attribute);
}

llvm::CallInst* LlvmCodeGenerator::EmitFunctionCall(
    const semantic::FunctionSymbol& symbol,
    const std::vector<llvm::Value*>& arguments, std::string_view name) {
    llvm::CallInst* call = _builder.CreateCall(
        _functions.at(&symbol), arguments, llvm::StringRef(name));
    if (!symbol.IsExtern()) return call;
    if (const auto attribute = ExternIntegerExtension(symbol.ReturnType()))
        call->addRetAttr(*attribute);
    for (std::size_t index = 0; index < symbol.ParameterTypes().size(); ++index)
        if (const auto attribute =
                ExternIntegerExtension(symbol.ParameterTypes()[index]))
            call->addParamAttr(index, *attribute);
    return call;
}

llvm::Value* LlvmCodeGenerator::EmitIndirectCall(
    const lowering::BoundIndirectCallExpression& expression,
    llvm::Value* destination) {
    llvm::Value* callable = EmitExpression(expression.Callee());
    if (callable == nullptr) return nullptr;
    const auto& signature = expression.Callee().Type().Signature();
    llvm::Value* invoke =
        _builder.CreateExtractValue(callable, 0, "function.invoke");
    llvm::Value* environment =
        _builder.CreateExtractValue(callable, 1, "function.environment");
    std::vector<llvm::Value*> arguments;
    arguments.reserve(expression.Arguments().size() + 2);
    if (signature.result.ContainsStructure()) {
        if (destination == nullptr) return nullptr;
        arguments.push_back(destination);
    }
    arguments.push_back(environment);
    for (std::size_t index = 0; index < expression.Arguments().size();
         ++index) {
        const auto& argument = *expression.Arguments()[index];
        llvm::Value* value = EmitExpression(argument);
        if (value == nullptr) return nullptr;
        value =
            CoerceValue(value, argument.Type(), signature.parameters[index]);
        if (value == nullptr) return nullptr;
        value = EmitValueCopy(value, signature.parameters[index], true);
        if (value == nullptr) return nullptr;
        arguments.push_back(value);
    }
    auto* call = _builder.CreateCall(
        MapCallableInvokeType(signature), invoke, arguments,
        signature.result.Kind() == semantic::SemanticTypeKind::Void ||
                signature.result.ContainsStructure()
            ? ""
            : "call.indirect");
    if (signature.isOnce)
        EmitValueDestroy(callable, expression.Callee().Type());
    return call;
}

bool LlvmCodeGenerator::DeclareFunctions(
    const lowering::BoundProgram& program) {
    for (const auto& function : program.Functions())
        _boundFunctions.emplace(&function->Symbol(), function.get());
    for (const std::unique_ptr<lowering::BoundFunction>& boundFunction :
         program.Functions()) {
        const semantic::FunctionSymbol& symbol = boundFunction->Symbol();
        llvm::FunctionType* type = MapFunctionType(*boundFunction);
        if (type == nullptr) return false;
        const std::string& linkName = symbol.LinkName();
        llvm::Function* function = _module->getFunction(linkName);
        if (function != nullptr && function->getFunctionType() != type)
            return Fail("conflicting extern declarations for symbol '" +
                        linkName + "'");
        if (function == nullptr)
            function = llvm::Function::Create(
                type, llvm::Function::ExternalLinkage, linkName, *_module);
        ApplyExternAttributes(symbol, *function);
        _functions.emplace(&symbol, function);
        // A Flecha void entry point has a defined successful process status.
        // Keep its language-level signature and synthesize the C ABI wrapper.
        if (!symbol.IsExtern() && symbol.Name() == "main" &&
            symbol.ReturnType().Kind() == semantic::SemanticTypeKind::Void &&
            symbol.ParameterTypes().empty()) {
            function->setName("$flecha$main");
            auto* entryType = llvm::FunctionType::get(
                llvm::Type::getInt32Ty(_context), false);
            auto* entry = llvm::Function::Create(
                entryType, llvm::Function::ExternalLinkage, "main", *_module);
            llvm::IRBuilder<> builder(
                llvm::BasicBlock::Create(_context, "entry", entry));
            builder.CreateCall(function);
            builder.CreateRet(
                llvm::ConstantInt::get(llvm::Type::getInt32Ty(_context), 0));
        }
    }
    return true;
}

llvm::Function* LlvmCodeGenerator::CallableAdapter(
    const semantic::FunctionSymbol& symbol) {
    const auto found = _callableAdapters.find(&symbol);
    if (found != _callableAdapters.end()) return found->second;
    auto* type = MapCallableInvokeType(symbol.Signature());
    if (type == nullptr) return nullptr;
    auto* adapter =
        llvm::Function::Create(type, llvm::Function::InternalLinkage,
                               symbol.LinkName() + ".$callable", *_module);
    _callableAdapters.emplace(&symbol, adapter);
    auto* entry = llvm::BasicBlock::Create(_context, "entry", adapter);
    llvm::IRBuilder<> builder(entry);
    std::vector<llvm::Value*> arguments;
    arguments.reserve(adapter->arg_size() - 1);
    std::size_t index = 0;
    if (symbol.ReturnType().ContainsStructure())
        arguments.push_back(adapter->getArg(index++));
    llvm::Value* environment = adapter->getArg(index++);
    environment->setName("environment");
    const auto bound = _boundFunctions.find(&symbol);
    if (bound != _boundFunctions.end() && !bound->second->Captures().empty())
        arguments.push_back(environment);
    for (; index < adapter->arg_size(); ++index)
        arguments.push_back(adapter->getArg(index));
    llvm::CallInst* call =
        builder.CreateCall(_functions.at(&symbol), arguments);
    if (symbol.IsExtern()) {
        if (const auto attribute = ExternIntegerExtension(symbol.ReturnType()))
            call->addRetAttr(*attribute);
        for (std::size_t i = 0; i < symbol.ParameterTypes().size(); ++i)
            if (const auto attribute =
                    ExternIntegerExtension(symbol.ParameterTypes()[i]))
                call->addParamAttr(
                    i + (symbol.ReturnType().ContainsStructure() ? 1 : 0),
                    *attribute);
    }
    if (call->getType()->isVoidTy())
        builder.CreateRetVoid();
    else
        builder.CreateRet(call);
    return adapter;
}

llvm::StructType* LlvmCodeGenerator::ClosureEnvironmentType(
    const lowering::BoundFunction& function) {
    const auto found = _closureEnvironmentTypes.find(&function.Symbol());
    if (found != _closureEnvironmentTypes.end()) return found->second;
    auto* environment = llvm::StructType::create(
        _context, "flecha.closure.env." + function.Symbol().LinkName());
    _closureEnvironmentTypes.emplace(&function.Symbol(), environment);
    std::vector<llvm::Type*> fields;
    fields.reserve(function.Captures().size() * 2);
    for (const auto& capture : function.Captures()) {
        const bool borrowedValue =
            capture.variable->Type().HasBorrowedLifetime();
        fields.push_back(CaptureStoredByValue(capture.mode) || borrowedValue
                             ? MapType(capture.variable->Type())
                             : llvm::PointerType::getUnqual(_context));
    }
    for (const auto& capture : function.Captures())
        if (capture.mode ==
                core::AnonymousFunctionExpression::CaptureMode::Own &&
            capture.variable->Type().ContainsStructure())
            fields.push_back(llvm::Type::getInt1Ty(_context));
    environment->setBody(fields);
    return environment;
}

namespace {

std::optional<unsigned> ClosureLifetimeField(
    const lowering::BoundFunction& function, std::size_t captureIndex) {
    unsigned field = static_cast<unsigned>(function.Captures().size());
    for (std::size_t index = 0; index < function.Captures().size(); ++index) {
        const auto& capture = function.Captures()[index];
        if (capture.mode !=
                core::AnonymousFunctionExpression::CaptureMode::Own ||
            !capture.variable->Type().ContainsStructure())
            continue;
        if (index == captureIndex) return field;
        ++field;
    }
    return std::nullopt;
}

}  // namespace

llvm::Function* LlvmCodeGenerator::ClosureDestroyer(
    const lowering::BoundFunction& function) {
    const auto found = _closureDestroyers.find(&function.Symbol());
    if (found != _closureDestroyers.end()) return found->second;
    llvm::Type* pointer = llvm::PointerType::getUnqual(_context);
    auto* type = llvm::FunctionType::get(llvm::Type::getVoidTy(_context),
                                         {pointer}, false);
    auto* destroyer = llvm::Function::Create(
        type, llvm::Function::InternalLinkage,
        function.Symbol().LinkName() + ".$closure.destroy", *_module);
    _closureDestroyers.emplace(&function.Symbol(), destroyer);

    const auto savedIP = _builder.saveIP();
    llvm::Function* savedFunction = _currentFunction;
    _currentFunction = destroyer;
    auto* entry = llvm::BasicBlock::Create(_context, "entry", destroyer);
    _builder.SetInsertPoint(entry);
    llvm::Value* environment = destroyer->getArg(0);
    environment->setName("environment");
    auto* environmentType = ClosureEnvironmentType(function);
    for (std::size_t index = function.Captures().size(); index > 0; --index) {
        const auto& capture = function.Captures()[index - 1];
        if (capture.mode ==
            core::AnonymousFunctionExpression::CaptureMode::Own) {
            llvm::Value* field = _builder.CreateStructGEP(
                environmentType, environment, index - 1);
            const semantic::SemanticType type = capture.variable->Type();
            if (type.Kind() == semantic::SemanticTypeKind::Pointer ||
                type.Kind() == semantic::SemanticTypeKind::OwnedBuffer)
                EmitFree(
                    _builder.CreateLoad(MapType(type), field, "owned.capture"),
                    type);
            else if (type.OwnsStorage()) {
                const auto lifetime = ClosureLifetimeField(function, index - 1);
                if (lifetime.has_value()) {
                    auto* destroy = llvm::BasicBlock::Create(
                        _context, "capture.destroy", _currentFunction);
                    auto* done = llvm::BasicBlock::Create(
                        _context, "capture.destroy.done", _currentFunction);
                    llvm::Value* live = _builder.CreateStructGEP(
                        environmentType, environment, *lifetime);
                    _builder.CreateCondBr(
                        _builder.CreateLoad(llvm::Type::getInt1Ty(_context),
                                            live),
                        destroy, done);
                    _builder.SetInsertPoint(destroy);
                    EmitDestroyAt(field, type);
                    _builder.CreateBr(done);
                    _builder.SetInsertPoint(done);
                } else {
                    EmitDestroyAt(field, type);
                }
            }
        } else if (capture.mode ==
                       core::AnonymousFunctionExpression::CaptureMode::Copy &&
                   capture.variable->Type().OwnsStorage())
            EmitDestroyAt(_builder.CreateStructGEP(environmentType, environment,
                                                   index - 1),
                          capture.variable->Type());
    }
    llvm::FunctionCallee release =
        _module->getOrInsertFunction("flecha_runtime_v1_free", type);
    _builder.CreateCall(release, {environment});
    _builder.CreateRetVoid();
    _currentFunction = savedFunction;
    _builder.restoreIP(savedIP);
    return destroyer;
}

llvm::Value* LlvmCodeGenerator::EmitClosure(
    const lowering::BoundFunctionExpression& expression) {
    const auto bound = _boundFunctions.find(&expression.Symbol());
    if (bound == _boundFunctions.end()) return nullptr;
    const lowering::BoundFunction& function = *bound->second;
    llvm::Value* callable = llvm::PoisonValue::get(MapType(expression.Type()));
    llvm::Function* adapter = CallableAdapter(expression.Symbol());
    if (adapter == nullptr) return nullptr;
    callable = _builder.CreateInsertValue(callable, adapter, 0);
    auto* pointerType = llvm::PointerType::getUnqual(_context);
    if (expression.Captures().empty()) {
        auto* null = llvm::ConstantPointerNull::get(pointerType);
        callable = _builder.CreateInsertValue(callable, null, 1);
        return _builder.CreateInsertValue(callable, null, 2, "function.value");
    }

    auto* environmentType = ClosureEnvironmentType(function);
    const llvm::DataLayout& layout = _module->getDataLayout();
    llvm::Type* sizeType =
        MapType(semantic::SemanticType(semantic::SemanticTypeKind::USize));
    llvm::FunctionCallee allocate = _module->getOrInsertFunction(
        "flecha_runtime_v1_alloc",
        llvm::FunctionType::get(pointerType, {sizeType, sizeType}, false));
    llvm::Value* environment = _builder.CreateCall(
        allocate,
        {llvm::ConstantInt::get(
             sizeType,
             layout.getTypeAllocSize(environmentType).getFixedValue()),
         llvm::ConstantInt::get(
             sizeType, layout.getABITypeAlign(environmentType).value())},
        "closure.environment");
    auto* initialize = llvm::BasicBlock::Create(_context, "closure.initialize",
                                                _currentFunction);
    auto* failed = llvm::BasicBlock::Create(_context, "closure.alloc.failed",
                                            _currentFunction);
    _builder.CreateCondBr(
        _builder.CreateICmpNE(environment,
                              llvm::ConstantPointerNull::get(pointerType)),
        initialize, failed);
    _builder.SetInsertPoint(failed);
    llvm::FunctionCallee panic = _module->getOrInsertFunction(
        "flecha_runtime_v1_allocation_failure",
        llvm::FunctionType::get(llvm::Type::getVoidTy(_context), false));
    _builder.CreateCall(panic);
    _builder.CreateUnreachable();
    _builder.SetInsertPoint(initialize);
    for (std::size_t index = 0; index < expression.Captures().size(); ++index) {
        const auto& capture = expression.Captures()[index];
        llvm::Value* value = nullptr;
        if (CaptureStoredByValue(capture.mode) ||
            capture.variable->Type().HasBorrowedLifetime()) {
            value = EmitExpression(*capture.value);
            if (value != nullptr &&
                capture.mode ==
                    core::AnonymousFunctionExpression::CaptureMode::Copy)
                value = EmitValueCopy(value, capture.variable->Type(), false);
        } else {
            value =
                EmitAddress(static_cast<const lowering::BoundPlaceExpression&>(
                    *capture.value));
        }
        if (value == nullptr) return nullptr;
        _builder.CreateStore(value, _builder.CreateStructGEP(
                                        environmentType, environment, index));
        if (const auto lifetime = ClosureLifetimeField(function, index))
            _builder.CreateStore(llvm::ConstantInt::getTrue(_context),
                                 _builder.CreateStructGEP(
                                     environmentType, environment, *lifetime));
    }
    callable = _builder.CreateInsertValue(callable, environment, 1);
    return _builder.CreateInsertValue(callable, ClosureDestroyer(function), 2,
                                      "closure.value");
}

llvm::AllocaInst* LlvmCodeGenerator::CreateEntryAlloca(
    const semantic::VariableSymbol& variable) {
    llvm::Type* type = MapType(variable.Type());
    if (type == nullptr) return nullptr;
    return CreateEntryStorage(type, variable.Name(),
                              variable.Type().OwnsStorage());
}

llvm::AllocaInst* LlvmCodeGenerator::CreateEntryStorage(llvm::Type* type,
                                                        std::string_view name,
                                                        bool initialize) {
    llvm::BasicBlock* activeBlock = _builder.GetInsertBlock();
    llvm::Instruction* insertionAnchor = nullptr;
    if (activeBlock != nullptr &&
        _builder.GetInsertPoint() != activeBlock->end()) {
        insertionAnchor = &*_builder.GetInsertPoint();
    }

    llvm::IRBuilder<> entryBuilder(_context);
    llvm::BasicBlock& entry = _currentFunction->getEntryBlock();
    entryBuilder.SetInsertPoint(&entry, entry.begin());
    llvm::AllocaInst* storage =
        entryBuilder.CreateAlloca(type, nullptr, std::string(name));
    if (initialize) {
        entryBuilder.CreateStore(llvm::Constant::getNullValue(type), storage);
    }

    // Prepending to an empty block can invalidate a builder's saved end
    // iterator (notably with LLVM 23). Re-establish the active insertion point
    // after every entry-block mutation.
    if (insertionAnchor != nullptr)
        _builder.SetInsertPoint(insertionAnchor);
    else if (activeBlock != nullptr)
        _builder.SetInsertPoint(activeBlock);
    return storage;
}

bool LlvmCodeGenerator::EmitFunction(
    const lowering::BoundFunction& boundFunction) {
    _currentFunction = _functions.at(&boundFunction.Symbol());
    _variables.clear();
    _lifetimeFlags.clear();
    _ownedCaptureStorage.clear();
    _returnStorage = nullptr;
    _positions.clear();
    _positionScopePaths.clear();
    _proceedTargets.clear();
    _breakTargets.clear();
    _cleanupScopes.clear();
    _cleanupRequests.clear();
    _blockScopes.clear();
    _freshOwnedStrings.clear();
    _freshOwnedValues.clear();
    _currentReturnType = boundFunction.Symbol().ReturnType();
    llvm::BasicBlock* entry =
        llvm::BasicBlock::Create(_context, "entry", _currentFunction);
    _builder.SetInsertPoint(entry);
    DeclarePositions(boundFunction.Body());
    std::vector<const lowering::BoundBlockStatement*> positionPath;
    CollectPositionScopePaths(boundFunction.Body(), positionPath);
    _cleanupScopes.push_back(
        std::make_shared<CleanupScope>(nullptr, std::vector<OwnedStorage>{}));

    std::size_t index = 0;
    for (llvm::Argument& argument : _currentFunction->args()) {
        if (_currentReturnType.ContainsStructure() &&
            _returnStorage == nullptr) {
            _returnStorage = &argument;
            argument.setName("result.storage");
            continue;
        }
        if (!boundFunction.Captures().empty() && index == 0 &&
            _variables.empty()) {
            argument.setName("environment");
            auto* environmentType = ClosureEnvironmentType(boundFunction);
            for (std::size_t captureIndex = 0;
                 captureIndex < boundFunction.Captures().size();
                 ++captureIndex) {
                const auto& capture = boundFunction.Captures()[captureIndex];
                llvm::Value* field = _builder.CreateStructGEP(
                    environmentType, &argument, captureIndex);
                llvm::Value* storage = field;
                if (!CaptureStoredByValue(capture.mode) &&
                    !capture.variable->Type().HasBorrowedLifetime())
                    storage = _builder.CreateLoad(
                        llvm::PointerType::getUnqual(_context), field,
                        capture.variable->Name() + ".capture");
                _variables.emplace(capture.variable, storage);
                if (capture.mode ==
                    core::AnonymousFunctionExpression::CaptureMode::Own)
                    _ownedCaptureStorage.insert(storage);
                if (const auto lifetime =
                        ClosureLifetimeField(boundFunction, captureIndex))
                    _lifetimeFlags[storage] = _builder.CreateStructGEP(
                        environmentType, &argument, *lifetime);
            }
            continue;
        }
        const semantic::VariableSymbol& parameter =
            *boundFunction.Parameters()[index++];
        argument.setName(parameter.Name());
        llvm::AllocaInst* storage = CreateEntryAlloca(parameter);
        if (storage == nullptr) return false;
        llvm::Value* parameterValue = &argument;
        if (parameter.Type().OwnsStorage() && parameter.Type().IsCopyable()) {
            parameterValue =
                EmitValueCopy(parameterValue, parameter.Type(), false);
        }
        if (parameter.Type().OwnsStorage()) {
            _cleanupScopes.back()->ownedValues.push_back(
                {storage, parameter.Type()});
        }
        _builder.CreateStore(parameterValue, storage);
        if (parameter.Type().ContainsStructure())
            _builder.CreateStore(llvm::ConstantInt::getTrue(_context),
                                 LifetimeFlag(storage));
        _variables.emplace(&parameter, storage);
    }

    if (!EmitBlock(boundFunction.Body())) return false;
    if (TerminatorOrNull(_builder.GetInsertBlock()) == nullptr) {
        if (boundFunction.Symbol().ReturnType().Kind() ==
            semantic::SemanticTypeKind::Void) {
            EmitCleanupsTo(0);
            _builder.CreateRetVoid();
        } else {
            _builder.CreateUnreachable();
        }
    }
    for (const CleanupRequest& request : _cleanupRequests) {
        llvm::BasicBlock* continuation = nullptr;
        const bool hasDefers =
            std::any_of(request.scopes.begin(), request.scopes.end(),
                        [](const auto& scope) {
                            if (scope->deferredHead != nullptr) return true;
                            for (const auto& storage : scope->ownedValues)
                                if (storage.type.ContainsStructure() ||
                                    storage.type.ContainsOwnedSlice() ||
                                    storage.type.Kind() ==
                                        semantic::SemanticTypeKind::Function)
                                    return true;
                            return false;
                        });
        if (hasDefers) {
            // Move the original continuation (return, jump, or fallthrough)
            // before emitting cleanup loops. It may initialize a jump target.
            continuation = request.block->splitBasicBlock(
                request.block->begin(), "cleanup.continue");
            TerminatorOrNull(request.block)->eraseFromParent();
            _builder.SetInsertPoint(request.block);
        } else {
            _builder.SetInsertPoint(request.block, request.block->begin());
        }
        for (auto scope = request.scopes.rbegin();
             scope != request.scopes.rend(); ++scope) {
            if (!EmitDeferredCalls(**scope)) return false;
            for (auto storage = (*scope)->ownedValues.rbegin();
                 storage != (*scope)->ownedValues.rend(); ++storage) {
                EmitValueStorageCleanup(storage->storage, storage->type);
            }
        }
        if (continuation != nullptr) _builder.CreateBr(continuation);
    }
    _cleanupRequests.clear();
    _cleanupScopes.clear();
    _currentFunction = nullptr;
    return true;
}

bool LlvmCodeGenerator::EmitBlock(const lowering::BoundBlockStatement& block) {
    _cleanupScopes.push_back(_blockScopes.at(&block));
    InitializeDeferredScope(*_cleanupScopes.back());
    for (const std::unique_ptr<lowering::BoundStatement>& statement :
         block.Statements()) {
        if (TerminatorOrNull(_builder.GetInsertBlock()) != nullptr &&
            statement->Kind() != lowering::BoundNodeKind::PositionStatement) {
            llvm::BasicBlock* unreachableBlock = llvm::BasicBlock::Create(
                _context, "unreachable", _currentFunction);
            _builder.SetInsertPoint(unreachableBlock);
        }
        if (!EmitStatement(*statement)) {
            _cleanupScopes.pop_back();
            return false;
        }
    }
    if (TerminatorOrNull(_builder.GetInsertBlock()) == nullptr) {
        EmitCleanupsTo(_cleanupScopes.size() - 1);
    }
    _cleanupScopes.pop_back();
    return true;
}

void LlvmCodeGenerator::CollectPositionScopePaths(
    const lowering::BoundBlockStatement& block,
    std::vector<const lowering::BoundBlockStatement*>& path) {
    path.push_back(&block);
    auto scope =
        std::make_shared<CleanupScope>(&block, std::vector<OwnedStorage>{});
    _blockScopes.emplace(&block, scope);
    for (const auto& statement : block.Statements()) {
        if (statement->Kind() != lowering::BoundNodeKind::DeferStatement)
            continue;
        const auto& deferred =
            static_cast<const lowering::BoundDeferStatement&>(*statement);
        const auto& call = deferred.Call();
        std::vector<semantic::SemanticType> captureTypes;
        std::vector<bool> captureOwnershipTransfers;
        std::vector<llvm::Type*> fields{llvm::PointerType::getUnqual(_context),
                                        llvm::Type::getInt32Ty(_context)};
        const auto addCapture = [&](const lowering::BoundExpression& expression,
                                    semantic::SemanticType type) {
            captureTypes.push_back(type);
            captureOwnershipTransfers.push_back(
                expression.Kind() == lowering::BoundNodeKind::MoveExpression ||
                expression.Kind() == lowering::BoundNodeKind::DirectExpression);
            fields.push_back(
                type.Kind() == semantic::SemanticTypeKind::String ||
                        type.Kind() == semantic::SemanticTypeKind::StringView
                    ? StringType()
                    : MapType(type));
        };
        const bool indirect =
            call.Kind() == lowering::BoundNodeKind::IndirectCallExpression;
        if (indirect) {
            const auto& value =
                static_cast<const lowering::BoundIndirectCallExpression&>(call);
            addCapture(value.Callee(), value.Callee().Type());
            for (std::size_t index = 0; index < value.Arguments().size();
                 ++index)
                addCapture(*value.Arguments()[index],
                           value.Callee().Type().Signature().parameters[index]);
        } else {
            const auto& direct =
                static_cast<const lowering::BoundCallExpression&>(call);
            for (std::size_t index = 0; index < direct.Arguments().size();
                 ++index) {
                const semantic::SemanticType type =
                    direct.Function().BuiltIn() ==
                            semantic::BuiltInFunction::None
                        ? direct.Function().ParameterTypes()[index]
                        : direct.Arguments()[index]->Type();
                addCapture(*direct.Arguments()[index], type);
            }
        }
        scope->deferredCalls.push_back(
            {&deferred, llvm::StructType::get(_context, fields),
             std::move(captureTypes), std::move(captureOwnershipTransfers),
             indirect});
    }
    if (!scope->deferredCalls.empty()) {
        scope->deferredHead = CreateTemporaryAlloca(
            llvm::PointerType::getUnqual(_context), "defer.head");
        scope->stackMark = CreateTemporaryAlloca(
            llvm::PointerType::getUnqual(_context), "defer.stack");
    }
    for (const std::unique_ptr<lowering::BoundStatement>& statement :
         block.Statements()) {
        if (statement->Kind() == lowering::BoundNodeKind::PositionStatement) {
            const lowering::BoundPositionStatement& position =
                static_cast<const lowering::BoundPositionStatement&>(
                    *statement);
            _positionScopePaths.emplace(position.Name(), path);
        } else if (statement->Kind() ==
                   lowering::BoundNodeKind::BlockStatement) {
            CollectPositionScopePaths(
                static_cast<const lowering::BoundBlockStatement&>(*statement),
                path);
        } else if (statement->Kind() == lowering::BoundNodeKind::IfStatement) {
            const lowering::BoundIfStatement& conditional =
                static_cast<const lowering::BoundIfStatement&>(*statement);
            CollectPositionScopePaths(conditional.ThenBlock(), path);
            if (conditional.ElseBlock() != nullptr) {
                CollectPositionScopePaths(*conditional.ElseBlock(), path);
            }
        } else if (statement->Kind() ==
                   lowering::BoundNodeKind::WhileStatement) {
            CollectPositionScopePaths(
                static_cast<const lowering::BoundWhileStatement&>(*statement)
                    .Body(),
                path);
        }
    }
    path.pop_back();
}

void LlvmCodeGenerator::DeclarePositions(
    const lowering::BoundBlockStatement& block) {
    for (const std::unique_ptr<lowering::BoundStatement>& statement :
         block.Statements()) {
        switch (statement->Kind()) {
            case lowering::BoundNodeKind::BlockStatement:
                DeclarePositions(
                    static_cast<const lowering::BoundBlockStatement&>(
                        *statement));
                break;
            case lowering::BoundNodeKind::PositionStatement: {
                const lowering::BoundPositionStatement& position =
                    static_cast<const lowering::BoundPositionStatement&>(
                        *statement);
                _positions.emplace(position.Name(),
                                   llvm::BasicBlock::Create(
                                       _context, "position." + position.Name(),
                                       _currentFunction));
                break;
            }
            case lowering::BoundNodeKind::IfStatement: {
                const lowering::BoundIfStatement& ifStatement =
                    static_cast<const lowering::BoundIfStatement&>(*statement);
                DeclarePositions(ifStatement.ThenBlock());
                if (ifStatement.ElseBlock() != nullptr) {
                    DeclarePositions(*ifStatement.ElseBlock());
                }
                break;
            }
            case lowering::BoundNodeKind::WhileStatement:
                DeclarePositions(
                    static_cast<const lowering::BoundWhileStatement&>(
                        *statement)
                        .Body());
                break;
            default:
                break;
        }
    }
}

bool LlvmCodeGenerator::EmitStatement(
    const lowering::BoundStatement& statement) {
    switch (statement.Kind()) {
        case lowering::BoundNodeKind::BlockStatement:
            return EmitBlock(
                static_cast<const lowering::BoundBlockStatement&>(statement));
        case lowering::BoundNodeKind::VariableDeclarationStatement: {
            const lowering::BoundVariableDeclarationStatement& declaration =
                static_cast<const lowering::BoundVariableDeclarationStatement&>(
                    statement);
            if (declaration.Variable().Type().ContainsStructure()) {
                auto* storage = CreateEntryAlloca(declaration.Variable());
                _variables[&declaration.Variable()] = storage;
                LifetimeFlag(storage);
                EmitValueStorageCleanup(storage, declaration.Variable().Type());
                _cleanupScopes.back()->ownedValues.push_back(
                    {storage, declaration.Variable().Type()});
                if (declaration.Initializer()) {
                    if (!EmitInto(*declaration.Initializer(), storage,
                                  declaration.Variable().Type()))
                        return false;
                    _builder.CreateStore(llvm::ConstantInt::getTrue(_context),
                                         LifetimeFlag(storage));
                }
                return true;
            }
            llvm::Value* value = nullptr;
            if (declaration.Initializer() != nullptr) {
                value = EmitExpression(*declaration.Initializer());
                if (value == nullptr) return false;
                value = CoerceValue(value, declaration.Initializer()->Type(),
                                    declaration.Variable().Type());
            }
            if (value != nullptr &&
                declaration.Variable().Type().OwnsStorage()) {
                value =
                    EmitValueCopy(value, declaration.Variable().Type(), false);
            }
            llvm::AllocaInst* storage =
                CreateEntryAlloca(declaration.Variable());
            if (storage == nullptr) return false;
            if (value != nullptr &&
                declaration.Variable().Type().OwnsStorage()) {
                EmitValueDestroy(
                    _builder.CreateLoad(MapType(declaration.Variable().Type()),
                                        storage, "value.previous"),
                    declaration.Variable().Type());
            }
            if (value != nullptr) _builder.CreateStore(value, storage);
            _variables.emplace(&declaration.Variable(), storage);
            if (declaration.Variable().Type().OwnsStorage()) {
                _cleanupScopes.back()->ownedValues.push_back(
                    {storage, declaration.Variable().Type()});
            }
            return true;
        }
        case lowering::BoundNodeKind::DestructureStatement:
            return EmitDestructure(
                static_cast<const lowering::BoundDestructureStatement&>(
                    statement));
        case lowering::BoundNodeKind::DeferStatement:
            return EmitDefer(
                static_cast<const lowering::BoundDeferStatement&>(statement));
        case lowering::BoundNodeKind::ExpressionStatement: {
            const auto& expression =
                static_cast<const lowering::BoundExpressionStatement&>(
                    statement)
                    .Expression();
            llvm::Value* value = EmitExpression(expression);
            if (value == nullptr) return false;
            if (!expression.Type().IsCopyable() &&
                (expression.Kind() == lowering::BoundNodeKind::MoveExpression ||
                 expression.Kind() ==
                     lowering::BoundNodeKind::DirectExpression))
                EmitValueDestroy(value, expression.Type());
            return true;
        }
        case lowering::BoundNodeKind::ReturnStatement: {
            const lowering::BoundReturnStatement& returnStatement =
                static_cast<const lowering::BoundReturnStatement&>(statement);
            if (returnStatement.Expression() == nullptr) {
                EmitCleanupsTo(0);
                _builder.CreateRetVoid();
                return true;
            }
            if (_currentReturnType.ContainsStructure()) {
                if (!EmitInto(*returnStatement.Expression(), _returnStorage,
                              _currentReturnType))
                    return false;
                EmitCleanupsTo(0);
                _builder.CreateRetVoid();
                return true;
            }
            llvm::Value* value = EmitExpression(*returnStatement.Expression());
            if (value == nullptr) return false;
            if (_currentReturnType.Kind() == semantic::SemanticTypeKind::Void) {
                EmitCleanupsTo(0);
                _builder.CreateRetVoid();
                return true;
            }
            const semantic::SemanticType sourceType =
                returnStatement.Expression()->Type();
            value = CoerceValue(value, sourceType, _currentReturnType);
            if (value != nullptr && _currentReturnType.OwnsStorage()) {
                value = EmitValueCopy(value, _currentReturnType, false);
            }
            if (value == nullptr) return false;
            EmitCleanupsTo(0);
            _builder.CreateRet(value);
            return true;
        }
        case lowering::BoundNodeKind::IfStatement:
            return EmitIf(
                static_cast<const lowering::BoundIfStatement&>(statement));
        case lowering::BoundNodeKind::WhileStatement:
            return EmitWhile(
                static_cast<const lowering::BoundWhileStatement&>(statement));
        case lowering::BoundNodeKind::ProceedStatement:
            EmitCleanupsTo(_proceedTargets.back().retainedScopeCount);
            _builder.CreateBr(_proceedTargets.back().block);
            return true;
        case lowering::BoundNodeKind::BreakStatement:
            EmitCleanupsTo(_breakTargets.back().retainedScopeCount);
            _builder.CreateBr(_breakTargets.back().block);
            return true;
        case lowering::BoundNodeKind::JumpToStatement: {
            const lowering::BoundJumpToStatement& jump =
                static_cast<const lowering::BoundJumpToStatement&>(statement);
            const std::vector<const lowering::BoundBlockStatement*>&
                targetPath = _positionScopePaths.at(jump.Position());
            std::size_t common = 0;
            while (common + 1 < _cleanupScopes.size() &&
                   common < targetPath.size() &&
                   _cleanupScopes[common + 1]->block == targetPath[common]) {
                ++common;
            }
            EmitCleanupsTo(common + 1);
            // A jump may enter a lexical scope without visiting its opening
            // brace. Initialize only newly entered scopes, retaining common
            // ancestors and their pending registrations.
            for (std::size_t index = common; index < targetPath.size(); ++index)
                InitializeDeferredScope(*_blockScopes.at(targetPath[index]));
            _builder.CreateBr(_positions.at(jump.Position()));
            return true;
        }
        case lowering::BoundNodeKind::PositionStatement:
            return EmitPosition(
                static_cast<const lowering::BoundPositionStatement&>(
                    statement));
        default:
            return Fail("unsupported bound statement");
    }
}

bool LlvmCodeGenerator::EmitIf(const lowering::BoundIfStatement& statement) {
    llvm::Value* condition = EmitExpression(statement.Condition());
    if (condition == nullptr) return false;

    llvm::BasicBlock* thenBlock =
        llvm::BasicBlock::Create(_context, "if.then", _currentFunction);
    llvm::BasicBlock* elseBlock =
        statement.ElseBlock() == nullptr
            ? nullptr
            : llvm::BasicBlock::Create(_context, "if.else", _currentFunction);
    llvm::BasicBlock* mergeBlock =
        llvm::BasicBlock::Create(_context, "if.end", _currentFunction);
    _builder.CreateCondBr(condition, thenBlock,
                          elseBlock == nullptr ? mergeBlock : elseBlock);

    _builder.SetInsertPoint(thenBlock);
    if (!EmitBlock(statement.ThenBlock())) return false;
    if (TerminatorOrNull(_builder.GetInsertBlock()) == nullptr) {
        _builder.CreateBr(mergeBlock);
    }

    if (elseBlock != nullptr) {
        _builder.SetInsertPoint(elseBlock);
        if (!EmitBlock(*statement.ElseBlock())) return false;
        if (TerminatorOrNull(_builder.GetInsertBlock()) == nullptr) {
            _builder.CreateBr(mergeBlock);
        }
    }
    _builder.SetInsertPoint(mergeBlock);
    return true;
}

bool LlvmCodeGenerator::EmitWhile(
    const lowering::BoundWhileStatement& statement) {
    llvm::BasicBlock* conditionBlock =
        llvm::BasicBlock::Create(_context, "while.condition", _currentFunction);
    llvm::BasicBlock* bodyBlock =
        llvm::BasicBlock::Create(_context, "while.body", _currentFunction);
    llvm::BasicBlock* endBlock =
        llvm::BasicBlock::Create(_context, "while.end", _currentFunction);
    llvm::BasicBlock* updateBlock =
        statement.Update() == nullptr
            ? conditionBlock
            : llvm::BasicBlock::Create(_context, "loop.update",
                                       _currentFunction);
    _builder.CreateBr(conditionBlock);

    _builder.SetInsertPoint(conditionBlock);
    llvm::Value* condition = EmitExpression(statement.Condition());
    if (condition == nullptr) return false;
    _builder.CreateCondBr(condition, bodyBlock, endBlock);

    _builder.SetInsertPoint(bodyBlock);
    const std::size_t retainedScopeCount = _cleanupScopes.size();
    _proceedTargets.push_back({updateBlock, retainedScopeCount});
    _breakTargets.push_back({endBlock, retainedScopeCount});
    if (!EmitBlock(statement.Body())) return false;
    _proceedTargets.pop_back();
    _breakTargets.pop_back();
    if (TerminatorOrNull(_builder.GetInsertBlock()) == nullptr) {
        _builder.CreateBr(updateBlock);
    }
    if (statement.Update() != nullptr) {
        _builder.SetInsertPoint(updateBlock);
        if (EmitExpression(*statement.Update()) == nullptr) return false;
        _builder.CreateBr(conditionBlock);
    }

    _builder.SetInsertPoint(endBlock);
    return true;
}

bool LlvmCodeGenerator::EmitPosition(
    const lowering::BoundPositionStatement& statement) {
    llvm::BasicBlock* target = _positions.at(statement.Name());
    llvm::BasicBlock* current = _builder.GetInsertBlock();
    if (current != target && TerminatorOrNull(current) == nullptr) {
        _builder.CreateBr(target);
    }
    _builder.SetInsertPoint(target);
    return true;
}

llvm::Value* LlvmCodeGenerator::EmitLiteral(
    const lowering::BoundLiteralExpression& expression) {
    llvm::Type* type = MapType(expression.Type());
    if (type == nullptr) return nullptr;
    if (expression.IsZeroValue() ||
        expression.Type().Kind() == semantic::SemanticTypeKind::Null)
        return llvm::Constant::getNullValue(type);
    if (expression.Type().Kind() == semantic::SemanticTypeKind::Bool) {
        return llvm::ConstantInt::get(type, expression.Value() == "true");
    }
    if (expression.Type().Kind() == semantic::SemanticTypeKind::Char) {
        const unsigned char value =
            expression.Value().empty()
                ? 0
                : static_cast<unsigned char>(expression.Value().front());
        return llvm::ConstantInt::get(type, value);
    }
    if (expression.Type().Kind() == semantic::SemanticTypeKind::StringView) {
        llvm::GlobalVariable* global =
            _builder.CreateGlobalString(expression.Value());
        llvm::Value* zero =
            llvm::ConstantInt::get(llvm::Type::getInt32Ty(_context), 0);
        llvm::Value* data = _builder.CreateInBoundsGEP(
            global->getValueType(), global, {zero, zero}, "stringv.data");
        llvm::StructType* type = StringViewType();
        llvm::Value* value = llvm::UndefValue::get(type);
        value = _builder.CreateInsertValue(value, data, 0, "stringv.data");
        llvm::Value* length = llvm::ConstantInt::get(type->getElementType(1),
                                                     expression.Value().size());
        return _builder.CreateInsertValue(value, length, 1, "stringv");
    }
    if (expression.Type().IsInteger() ||
        expression.Type().Kind() == semantic::SemanticTypeKind::Enum ||
        expression.Type().Kind() == semantic::SemanticTypeKind::NamedError) {
        return llvm::ConstantInt::get(
            type,
            llvm::APInt(type->getIntegerBitWidth(), expression.Value(), 10));
    }
    if (expression.Type().IsFloating()) {
        char* end = nullptr;
        const double value = std::strtod(expression.Value().c_str(), &end);
        if (end == expression.Value().c_str() || *end != '\0') {
            Fail("invalid floating-point literal");
            return nullptr;
        }
        return llvm::ConstantFP::get(type, value);
    }
    Fail("unsupported LLVM literal type");
    return nullptr;
}

llvm::Value* LlvmCodeGenerator::EmitUnary(
    const lowering::BoundUnaryExpression& expression) {
    llvm::Value* operand = EmitExpression(expression.Operand());
    if (operand == nullptr) return nullptr;
    switch (expression.OperatorType()) {
        case TokenType::Add:
            return operand;
        case TokenType::Sub:
            return expression.Type().IsFloating()
                       ? _builder.CreateFNeg(operand, "fneg")
                       : _builder.CreateNeg(operand, "neg");
        case TokenType::Bang:
            return _builder.CreateNot(operand, "not");
        case TokenType::BitNot:
            return _builder.CreateNot(operand, "bitnot");
        default:
            Fail("unsupported LLVM unary operator");
            return nullptr;
    }
}

llvm::Value* LlvmCodeGenerator::EmitCast(
    const lowering::BoundCastExpression& expression) {
    llvm::Value* value = EmitExpression(expression.Value());
    if (value == nullptr) return nullptr;
    llvm::Type* targetType = MapType(expression.Type());
    if (targetType == nullptr) return nullptr;
    const semantic::SemanticType sourceType = expression.Value().Type();
    if (sourceType.Kind() == semantic::SemanticTypeKind::String &&
        expression.Type().Kind() == semantic::SemanticTypeKind::String) {
        return EmitOwnedString(value, sourceType);
    }
    if (sourceType.WithoutConst() == expression.Type().WithoutConst())
        return value;

    if (expression.Type().Kind() == semantic::SemanticTypeKind::String) {
        return EmitOwnedString(value, sourceType);
    }
    if (sourceType.Kind() == semantic::SemanticTypeKind::String &&
        expression.Type().Kind() == semantic::SemanticTypeKind::StringView) {
        return EmitStringView(value, sourceType);
    }

    if (sourceType.IsInteger() && expression.Type().IsInteger()) {
        return _builder.CreateIntCast(value, targetType,
                                      !IsUnsigned(sourceType), "intcast");
    }
    if (sourceType.IsInteger() && expression.Type().IsFloating()) {
        return IsUnsigned(sourceType)
                   ? _builder.CreateUIToFP(value, targetType, "uitofp")
                   : _builder.CreateSIToFP(value, targetType, "sitofp");
    }
    if (sourceType.IsFloating() && expression.Type().IsInteger()) {
        return IsUnsigned(expression.Type())
                   ? _builder.CreateFPToUI(value, targetType, "fptoui")
                   : _builder.CreateFPToSI(value, targetType, "fptosi");
    }
    if (sourceType.IsFloating() && expression.Type().IsFloating()) {
        return _builder.CreateFPCast(value, targetType, "fpcast");
    }
    Fail("unsupported LLVM cast");
    return nullptr;
}

llvm::Value* LlvmCodeGenerator::EmitBooleanLogical(
    const lowering::BoundBinaryExpression& expression) {
    llvm::Value* left = EmitExpression(expression.Left());
    if (left == nullptr) return nullptr;
    llvm::BasicBlock* leftBlock = _builder.GetInsertBlock();
    llvm::BasicBlock* rightBlock =
        llvm::BasicBlock::Create(_context, "logic.right", _currentFunction);
    llvm::BasicBlock* mergeBlock =
        llvm::BasicBlock::Create(_context, "logic.end", _currentFunction);
    const bool isAnd = expression.OperatorType() == TokenType::And;
    _builder.CreateCondBr(left, isAnd ? rightBlock : mergeBlock,
                          isAnd ? mergeBlock : rightBlock);
    _builder.SetInsertPoint(rightBlock);
    llvm::Value* right = EmitExpression(expression.Right());
    if (right == nullptr) return nullptr;
    llvm::BasicBlock* completedRightBlock = _builder.GetInsertBlock();
    _builder.CreateBr(mergeBlock);
    _builder.SetInsertPoint(mergeBlock);
    llvm::PHINode* result =
        _builder.CreatePHI(llvm::Type::getInt1Ty(_context), 2, "logic");
    result->addIncoming(llvm::ConstantInt::getBool(_context, !isAnd),
                        leftBlock);
    result->addIncoming(right, completedRightBlock);
    return result;
}

llvm::Value* LlvmCodeGenerator::EmitValueEquality(llvm::Value* left,
                                                  llvm::Value* right,
                                                  semantic::SemanticType type) {
    using Kind = semantic::SemanticTypeKind;
    if (type.Kind() == Kind::String || type.Kind() == Kind::StringView)
        return EmitStringViewEquality(EmitStringView(left, type),
                                      EmitStringView(right, type), false);
    if (type.Kind() == Kind::Structure) {
        llvm::Value* equal = llvm::ConstantInt::getTrue(_context);
        for (std::size_t i = 0; i < type.Structure()->fields.size(); ++i) {
            llvm::Value* field =
                EmitValueEquality(_builder.CreateExtractValue(left, i),
                                  _builder.CreateExtractValue(right, i),
                                  type.Structure()->fields[i].type);
            if (!field) return nullptr;
            equal = _builder.CreateAnd(equal, field, "structure.field.equal");
        }
        return equal;
    }
    if (type.Kind() == Kind::Array) {
        llvm::Value* equal = llvm::ConstantInt::getTrue(_context);
        for (std::uint64_t i = 0; i < type.Length(); ++i) {
            llvm::Value* item = EmitValueEquality(
                _builder.CreateExtractValue(left, {static_cast<unsigned>(i)}),
                _builder.CreateExtractValue(right, {static_cast<unsigned>(i)}),
                *type.Target());
            if (!item) return nullptr;
            equal = _builder.CreateAnd(equal, item, "array.element.equal");
        }
        return equal;
    }
    if (type.Kind() == Kind::Tuple) {
        llvm::Value* equal = llvm::ConstantInt::getTrue(_context);
        for (std::size_t i = 0; i < type.Elements().size(); ++i) {
            llvm::Value* field = EmitValueEquality(
                _builder.CreateExtractValue(left, i),
                _builder.CreateExtractValue(right, i), type.Elements()[i]);
            if (!field) return nullptr;
            equal = _builder.CreateAnd(equal, field, "tuple.field.equal");
        }
        return equal;
    }
    if (type.Kind() == Kind::ErrorValue) {
        llvm::Value* sameFamily =
            _builder.CreateICmpEQ(_builder.CreateExtractValue(left, 0),
                                  _builder.CreateExtractValue(right, 0));
        llvm::Value* sameCode =
            _builder.CreateICmpEQ(_builder.CreateExtractValue(left, 1),
                                  _builder.CreateExtractValue(right, 1));
        return _builder.CreateAnd(sameFamily, sameCode, "error.equal");
    }
    if (type.IsFloating()) return _builder.CreateFCmpOEQ(left, right, "feq");
    return _builder.CreateICmpEQ(left, right, "eq");
}

llvm::Value* LlvmCodeGenerator::EmitBinary(
    const lowering::BoundBinaryExpression& expression) {
    if (expression.Type().Kind() == semantic::SemanticTypeKind::Bool &&
        (expression.OperatorType() == TokenType::And ||
         expression.OperatorType() == TokenType::Or)) {
        return EmitBooleanLogical(expression);
    }
    const auto leftType = expression.Left().Type();
    const auto rightType = expression.Right().Type();
    const bool strict = expression.OperatorType() == TokenType::StrictEqual ||
                        expression.OperatorType() == TokenType::StrictNotEqual;
    const bool leftNone = leftType.Kind() == semantic::SemanticTypeKind::None;
    const bool rightNone = rightType.Kind() == semantic::SemanticTypeKind::None;
    if ((strict && (leftNone || rightNone)) || (leftNone && rightNone)) {
        // none has no standalone runtime representation. Its type decides
        // strict comparisons, but the other operand must still be evaluated.
        if (!leftNone && EmitExpression(expression.Left()) == nullptr)
            return nullptr;
        if (!rightNone && EmitExpression(expression.Right()) == nullptr)
            return nullptr;
        const bool equal = leftNone && rightNone;
        const bool negate =
            expression.OperatorType() == TokenType::NotEqual ||
            expression.OperatorType() == TokenType::StrictNotEqual;
        return llvm::ConstantInt::get(llvm::Type::getInt1Ty(_context),
                                      negate ? !equal : equal);
    }
    if (strict && (leftType.IsErrorValue() || rightType.IsErrorValue()) &&
        leftType != rightType) {
        // Preserve operand effects even when differing static types decide
        // equality.
        if (leftType.Kind() != semantic::SemanticTypeKind::None &&
            EmitExpression(expression.Left()) == nullptr)
            return nullptr;
        if (rightType.Kind() != semantic::SemanticTypeKind::None &&
            EmitExpression(expression.Right()) == nullptr)
            return nullptr;
        return llvm::ConstantInt::get(
            llvm::Type::getInt1Ty(_context),
            expression.OperatorType() == TokenType::StrictNotEqual);
    }
    llvm::Value* left = EmitExpression(expression.Left());
    llvm::Value* right = EmitExpression(expression.Right());
    if (left == nullptr || right == nullptr) return nullptr;
    if (leftType.Kind() == semantic::SemanticTypeKind::OwnedBuffer ||
        leftType.Kind() == semantic::SemanticTypeKind::OwnedSlice ||
        rightType.Kind() == semantic::SemanticTypeKind::OwnedBuffer ||
        rightType.Kind() == semantic::SemanticTypeKind::OwnedSlice) {
        left = _builder.CreateExtractValue(left, 0, "owner.left.data");
        right = _builder.CreateExtractValue(right, 0, "owner.right.data");
        llvm::Value* equal = _builder.CreateICmpEQ(left, right, "owner.equal");
        return expression.OperatorType() == TokenType::NotEqual
                   ? _builder.CreateNot(equal, "owner.not_equal")
                   : equal;
    }
    if (leftType.Kind() == semantic::SemanticTypeKind::Structure &&
        rightType.Kind() == semantic::SemanticTypeKind::Structure &&
        (expression.OperatorType() == TokenType::Equal ||
         expression.OperatorType() == TokenType::NotEqual ||
         expression.OperatorType() == TokenType::StrictEqual ||
         expression.OperatorType() == TokenType::StrictNotEqual)) {
        llvm::Value* equal = EmitValueEquality(left, right, leftType);
        if (!equal) return nullptr;
        return expression.OperatorType() == TokenType::NotEqual ||
                       expression.OperatorType() == TokenType::StrictNotEqual
                   ? _builder.CreateNot(equal, "structure.not_equal")
                   : equal;
    }
    if (leftType.Kind() == semantic::SemanticTypeKind::ErrorValue ||
        rightType.Kind() == semantic::SemanticTypeKind::ErrorValue) {
        const semantic::SemanticType generic(
            semantic::SemanticTypeKind::ErrorValue);
        left = CoerceValue(left, leftType, generic);
        right = CoerceValue(right, rightType, generic);
        if (left == nullptr || right == nullptr) return nullptr;
        llvm::Value* sameFamily =
            _builder.CreateICmpEQ(_builder.CreateExtractValue(left, 0),
                                  _builder.CreateExtractValue(right, 0));
        llvm::Value* sameCode =
            _builder.CreateICmpEQ(_builder.CreateExtractValue(left, 1),
                                  _builder.CreateExtractValue(right, 1));
        llvm::Value* equal =
            _builder.CreateAnd(sameFamily, sameCode, "error.equal");
        return expression.OperatorType() == TokenType::NotEqual ||
                       expression.OperatorType() == TokenType::StrictNotEqual
                   ? _builder.CreateNot(equal)
                   : equal;
    }
    const bool leftString =
        expression.Left().Type().Kind() ==
            semantic::SemanticTypeKind::StringView ||
        expression.Left().Type().Kind() == semantic::SemanticTypeKind::String;
    const bool rightString =
        expression.Right().Type().Kind() ==
            semantic::SemanticTypeKind::StringView ||
        expression.Right().Type().Kind() == semantic::SemanticTypeKind::String;
    if (leftString && rightString &&
        expression.OperatorType() == TokenType::Add) {
        return EmitStringConcat(left, expression.Left().Type(), right,
                                expression.Right().Type());
    }
    if (leftString && rightString &&
        (expression.OperatorType() == TokenType::Equal ||
         expression.OperatorType() == TokenType::NotEqual ||
         expression.OperatorType() == TokenType::StrictEqual ||
         expression.OperatorType() == TokenType::StrictNotEqual)) {
        const bool negate =
            expression.OperatorType() == TokenType::NotEqual ||
            expression.OperatorType() == TokenType::StrictNotEqual;
        return EmitStringViewEquality(
            EmitStringView(left, expression.Left().Type()),
            EmitStringView(right, expression.Right().Type()), negate);
    }
    const bool floating = expression.Left().Type().IsFloating();
    const bool unsignedValue = IsUnsigned(expression.Left().Type());
    if (expression.OperatorType() == TokenType::ShiftLeft ||
        expression.OperatorType() == TokenType::ShiftRight) {
        auto* valueType = llvm::cast<llvm::IntegerType>(left->getType());
        auto* countType = llvm::cast<llvm::IntegerType>(right->getType());
        const unsigned width = valueType->getBitWidth();
        llvm::Value* oversized = _builder.CreateICmpUGE(
            right, llvm::ConstantInt::get(countType, width), "shift.oversized");
        llvm::Value* safeCount = _builder.CreateSelect(
            oversized, llvm::ConstantInt::get(countType, 0), right,
            "shift.safe_count");
        safeCount =
            _builder.CreateIntCast(safeCount, valueType, false, "shift.count");
        llvm::Value* shifted = nullptr;
        llvm::Value* oversizedValue = llvm::ConstantInt::get(valueType, 0);
        if (expression.OperatorType() == TokenType::ShiftLeft) {
            shifted = _builder.CreateShl(left, safeCount, "shl");
        } else if (unsignedValue) {
            shifted = _builder.CreateLShr(left, safeCount, "lshr");
        } else {
            shifted = _builder.CreateAShr(left, safeCount, "ashr");
            oversizedValue = _builder.CreateAShr(
                left, llvm::ConstantInt::get(valueType, width - 1),
                "shift.sign_fill");
        }
        return _builder.CreateSelect(oversized, oversizedValue, shifted,
                                     "shift.result");
    }
    switch (expression.OperatorType()) {
        case TokenType::Add:
            return floating ? _builder.CreateFAdd(left, right, "fadd")
                            : _builder.CreateAdd(left, right, "add");
        case TokenType::Sub:
            return floating ? _builder.CreateFSub(left, right, "fsub")
                            : _builder.CreateSub(left, right, "sub");
        case TokenType::Mul:
            return floating ? _builder.CreateFMul(left, right, "fmul")
                            : _builder.CreateMul(left, right, "mul");
        case TokenType::Div:
            if (floating) return _builder.CreateFDiv(left, right, "fdiv");
            return unsignedValue ? _builder.CreateUDiv(left, right, "udiv")
                                 : _builder.CreateSDiv(left, right, "sdiv");
        case TokenType::Mod:
            return unsignedValue ? _builder.CreateURem(left, right, "urem")
                                 : _builder.CreateSRem(left, right, "srem");
        case TokenType::And:
        case TokenType::Ampersand:
            return _builder.CreateAnd(left, right, "and");
        case TokenType::Or:
        case TokenType::BitOr:
            return _builder.CreateOr(left, right, "or");
        case TokenType::Xor:
            return _builder.CreateXor(left, right, "xor");
        case TokenType::Equal:
        case TokenType::StrictEqual:
            return floating ? _builder.CreateFCmpOEQ(left, right, "feq")
                            : _builder.CreateICmpEQ(left, right, "eq");
        case TokenType::NotEqual:
        case TokenType::StrictNotEqual:
            return floating ? _builder.CreateFCmpUNE(left, right, "fne")
                            : _builder.CreateICmpNE(left, right, "ne");
        case TokenType::Less:
            if (floating) return _builder.CreateFCmpOLT(left, right, "flt");
            return unsignedValue ? _builder.CreateICmpULT(left, right, "ult")
                                 : _builder.CreateICmpSLT(left, right, "slt");
        case TokenType::LessEqual:
            if (floating) return _builder.CreateFCmpOLE(left, right, "fle");
            return unsignedValue ? _builder.CreateICmpULE(left, right, "ule")
                                 : _builder.CreateICmpSLE(left, right, "sle");
        case TokenType::Greater:
            if (floating) return _builder.CreateFCmpOGT(left, right, "fgt");
            return unsignedValue ? _builder.CreateICmpUGT(left, right, "ugt")
                                 : _builder.CreateICmpSGT(left, right, "sgt");
        case TokenType::GreaterEqual:
            if (floating) return _builder.CreateFCmpOGE(left, right, "fge");
            return unsignedValue ? _builder.CreateICmpUGE(left, right, "uge")
                                 : _builder.CreateICmpSGE(left, right, "sge");
        default:
            Fail("unsupported LLVM binary operator");
            return nullptr;
    }
}

llvm::Value* LlvmCodeGenerator::EmitOverflowingArithmetic(
    const lowering::BoundOverflowingArithmeticExpression& expression) {
    llvm::Value* left = EmitExpression(expression.Left());
    llvm::Value* right = EmitExpression(expression.Right());
    if (left == nullptr || right == nullptr) return nullptr;

    const bool isUnsigned = IsUnsigned(expression.Left().Type());
    llvm::Intrinsic::ID intrinsic = llvm::Intrinsic::not_intrinsic;
    std::string_view name;
    switch (expression.Operation()) {
        case lowering::OverflowingArithmeticOperation::Add:
            intrinsic = isUnsigned ? llvm::Intrinsic::uadd_with_overflow
                                   : llvm::Intrinsic::sadd_with_overflow;
            name = "overflowing.add";
            break;
        case lowering::OverflowingArithmeticOperation::Subtract:
            intrinsic = isUnsigned ? llvm::Intrinsic::usub_with_overflow
                                   : llvm::Intrinsic::ssub_with_overflow;
            name = "overflowing.sub";
            break;
        case lowering::OverflowingArithmeticOperation::Multiply:
            intrinsic = isUnsigned ? llvm::Intrinsic::umul_with_overflow
                                   : llvm::Intrinsic::smul_with_overflow;
            name = "overflowing.mul";
            break;
    }
#if LLVM_VERSION_MAJOR >= 20
    llvm::Function* function = llvm::Intrinsic::getOrInsertDeclaration(
        _module.get(), intrinsic, {left->getType()});
#else
    llvm::Function* function = llvm::Intrinsic::getDeclaration(
        _module.get(), intrinsic, {left->getType()});
#endif
    return _builder.CreateCall(function, {left, right}, name);
}

llvm::Value* LlvmCodeGenerator::EmitIntegerLimit(
    const lowering::BoundIntegerLimitExpression& expression) {
    llvm::Type* type = MapType(expression.Type());
    if (type == nullptr || !type->isIntegerTy()) return nullptr;
    const unsigned bits = type->getIntegerBitWidth();
    const bool isUnsigned = IsUnsigned(expression.Type());
    llvm::APInt value = llvm::APInt::getZero(bits);
    if (expression.IsMaximum())
        value = isUnsigned ? llvm::APInt::getAllOnes(bits)
                           : llvm::APInt::getSignedMaxValue(bits);
    else if (!isUnsigned)
        value = llvm::APInt::getSignedMinValue(bits);
    return llvm::ConstantInt::get(type, value);
}

llvm::Value* LlvmCodeGenerator::EmitFloatingConstant(
    const lowering::BoundFloatingConstantExpression& expression) {
    llvm::Type* type = MapType(expression.Type());
    if (type == nullptr || !type->isFloatingPointTy()) return nullptr;

    const bool single =
        expression.Type().Kind() == semantic::SemanticTypeKind::F32;
    const llvm::fltSemantics& semantics =
        single ? llvm::APFloat::IEEEsingle() : llvm::APFloat::IEEEdouble();
    switch (expression.Constant()) {
        case lowering::FloatingConstant::NaN:
            return llvm::ConstantFP::getNaN(type);
        case lowering::FloatingConstant::Infinity:
            return llvm::ConstantFP::getInfinity(type);
        case lowering::FloatingConstant::NegativeInfinity:
            return llvm::ConstantFP::getInfinity(type, true);
        case lowering::FloatingConstant::Minimum:
            return llvm::ConstantFP::get(
                type, llvm::APFloat::getLargest(semantics, true));
        case lowering::FloatingConstant::Maximum:
            return llvm::ConstantFP::get(type,
                                         llvm::APFloat::getLargest(semantics));
        case lowering::FloatingConstant::MinimumPositive:
            return llvm::ConstantFP::get(
                type, llvm::APFloat::getSmallestNormalized(semantics));
        case lowering::FloatingConstant::Epsilon: {
            const llvm::APInt bits =
                single ? llvm::APInt(32, UINT64_C(0x34000000))
                       : llvm::APInt(64, UINT64_C(0x3cb0000000000000));
            return llvm::ConstantFP::get(type, llvm::APFloat(semantics, bits));
        }
    }
    return nullptr;
}

llvm::Value* LlvmCodeGenerator::EmitStringConcat(
    llvm::Value* left, semantic::SemanticType leftType, llvm::Value* right,
    semantic::SemanticType rightType) {
    llvm::Value* leftView = EmitStringView(left, leftType);
    llvm::Value* rightView = EmitStringView(right, rightType);
    if (leftView == nullptr || rightView == nullptr) return nullptr;

    llvm::Value* leftData =
        _builder.CreateExtractValue(leftView, 0, "concat.left.data");
    llvm::Value* leftLength =
        _builder.CreateExtractValue(leftView, 1, "concat.left.length");
    llvm::Value* rightData =
        _builder.CreateExtractValue(rightView, 0, "concat.right.data");
    llvm::Value* rightLength =
        _builder.CreateExtractValue(rightView, 1, "concat.right.length");
    llvm::StructType* resultType = StringType();
    llvm::AllocaInst* output = CreateOwnedStringTemporary("concat.result");
    EmitStringStorageCleanup(output);
    llvm::FunctionType* concatType = llvm::FunctionType::get(
        llvm::Type::getVoidTy(_context),
        {output->getType(), leftData->getType(), leftLength->getType(),
         rightData->getType(), rightLength->getType()},
        false);
    llvm::FunctionCallee concat = _module->getOrInsertFunction(
        "flecha_runtime_v1_string_concat", concatType);
    _builder.CreateCall(concat,
                        {output, leftData, leftLength, rightData, rightLength});
    llvm::Value* result = _builder.CreateLoad(resultType, output, "concat");
    _freshOwnedStrings.emplace(result, output);
    return result;
}

llvm::Value* LlvmCodeGenerator::EmitStringViewEquality(llvm::Value* left,
                                                       llvm::Value* right,
                                                       bool negate) {
    llvm::Value* leftData = _builder.CreateExtractValue(left, 0, "left.data");
    llvm::Value* leftLength =
        _builder.CreateExtractValue(left, 1, "left.length");
    llvm::Value* rightData =
        _builder.CreateExtractValue(right, 0, "right.data");
    llvm::Value* rightLength =
        _builder.CreateExtractValue(right, 1, "right.length");
    llvm::Value* sameLength =
        _builder.CreateICmpEQ(leftLength, rightLength, "same.length");

    llvm::BasicBlock* lengthBlock = _builder.GetInsertBlock();
    llvm::BasicBlock* contentBlock =
        llvm::BasicBlock::Create(_context, "stringv.content", _currentFunction);
    llvm::BasicBlock* mergeBlock =
        llvm::BasicBlock::Create(_context, "stringv.end", _currentFunction);
    _builder.CreateCondBr(sameLength, contentBlock, mergeBlock);

    _builder.SetInsertPoint(contentBlock);
    llvm::Type* lengthType = leftLength->getType();
    llvm::FunctionType* compareType = llvm::FunctionType::get(
        llvm::Type::getInt32Ty(_context),
        {llvm::PointerType::getUnqual(_context),
         llvm::PointerType::getUnqual(_context), lengthType},
        false);
    llvm::FunctionCallee compare =
        _module->getOrInsertFunction("memcmp", compareType);
    llvm::Value* comparison = _builder.CreateCall(
        compare, {leftData, rightData, leftLength}, "stringv.compare");
    llvm::Value* sameContent = _builder.CreateICmpEQ(
        comparison, llvm::ConstantInt::get(llvm::Type::getInt32Ty(_context), 0),
        "same.content");
    _builder.CreateBr(mergeBlock);
    llvm::BasicBlock* completedContentBlock = _builder.GetInsertBlock();

    _builder.SetInsertPoint(mergeBlock);
    llvm::PHINode* equal =
        _builder.CreatePHI(llvm::Type::getInt1Ty(_context), 2, "stringv.equal");
    equal->addIncoming(llvm::ConstantInt::getFalse(_context), lengthBlock);
    equal->addIncoming(sameContent, completedContentBlock);
    return negate ? _builder.CreateNot(equal, "stringv.not_equal") : equal;
}

llvm::AllocaInst* LlvmCodeGenerator::CreateTemporaryAlloca(
    llvm::Type* type, std::string_view name) {
    return CreateEntryStorage(type, name, false);
}

llvm::Value* LlvmCodeGenerator::EmitArray(
    const lowering::BoundArrayExpression& expression) {
    llvm::Type* type = MapType(expression.Type());
    if (!expression.IsRepeat()) {
        llvm::Value* result = llvm::Constant::getNullValue(type);
        unsigned index = 0;
        for (const auto& element : expression.Elements()) {
            llvm::Value* value = EmitExpression(*element);
            if (value == nullptr) return nullptr;
            value = CoerceValue(value, element->Type(),
                                *expression.Type().Target());
            if (value == nullptr) return nullptr;
            result = _builder.CreateInsertValue(result, value, index++,
                                                "array.element");
        }
        return result;
    }
    // Evaluate the repeated initializer once, including for a zero-length
    // array.
    const lowering::BoundExpression& element = *expression.Elements().front();
    llvm::Value* value = EmitExpression(element);
    if (value == nullptr) return nullptr;
    value = CoerceValue(value, element.Type(), *expression.Type().Target());
    if (value == nullptr) return nullptr;
    if (expression.Type().Length() == 0)
        return llvm::Constant::getNullValue(type);
    llvm::AllocaInst* storage =
        CreateTemporaryAlloca(type, "array.repeat.storage");
    llvm::Type* sizeType =
        MapType(semantic::SemanticType(semantic::SemanticTypeKind::USize));
    llvm::BasicBlock* before = _builder.GetInsertBlock();
    llvm::BasicBlock* body =
        llvm::BasicBlock::Create(_context, "array.repeat", _currentFunction);
    llvm::BasicBlock* done = llvm::BasicBlock::Create(
        _context, "array.repeat.end", _currentFunction);
    _builder.CreateBr(body);
    _builder.SetInsertPoint(body);
    llvm::PHINode* index =
        _builder.CreatePHI(sizeType, 2, "array.repeat.index");
    index->addIncoming(llvm::ConstantInt::get(sizeType, 0), before);
    llvm::Value* address = _builder.CreateInBoundsGEP(
        type, storage, {llvm::ConstantInt::get(sizeType, 0), index});
    _builder.CreateStore(value, address);
    llvm::Value* next =
        _builder.CreateAdd(index, llvm::ConstantInt::get(sizeType, 1));
    index->addIncoming(next, body);
    _builder.CreateCondBr(
        _builder.CreateICmpULT(
            next, llvm::ConstantInt::get(sizeType, expression.Type().Length())),
        body, done);
    _builder.SetInsertPoint(done);
    return _builder.CreateLoad(type, storage, "array.repeat.value");
}

llvm::Value* LlvmCodeGenerator::EmitArrayStorage(
    const lowering::BoundExpression& expression) {
    if (expression.Kind() == lowering::BoundNodeKind::VariableExpression ||
        expression.Kind() == lowering::BoundNodeKind::DereferenceExpression ||
        expression.Kind() == lowering::BoundNodeKind::ArrayElementExpression ||
        expression.Kind() == lowering::BoundNodeKind::FieldExpression)
        return EmitAddress(
            static_cast<const lowering::BoundPlaceExpression&>(expression));
    llvm::Value* value = EmitExpression(expression);
    if (value == nullptr) return nullptr;
    llvm::AllocaInst* storage =
        CreateTemporaryAlloca(MapType(expression.Type()), "array.temporary");
    _builder.CreateStore(value, storage);
    return storage;
}

void LlvmCodeGenerator::EmitBoundsCheck(llvm::Value* condition) {
    llvm::BasicBlock* valid =
        llvm::BasicBlock::Create(_context, "array.in.bounds", _currentFunction);
    llvm::BasicBlock* invalid = llvm::BasicBlock::Create(
        _context, "array.out.of.bounds", _currentFunction);
    _builder.CreateCondBr(condition, valid, invalid);
    _builder.SetInsertPoint(invalid);
    llvm::FunctionCallee trap = _module->getOrInsertFunction(
        "llvm.trap",
        llvm::FunctionType::get(llvm::Type::getVoidTy(_context), false));
    _builder.CreateCall(trap);
    _builder.CreateUnreachable();
    _builder.SetInsertPoint(valid);
}

llvm::Value* LlvmCodeGenerator::EmitSlice(
    const lowering::BoundSliceExpression& expression) {
    llvm::Type* sizeType =
        MapType(semantic::SemanticType(semantic::SemanticTypeKind::USize));
    llvm::Value* zero = llvm::ConstantInt::get(sizeType, 0);
    llvm::Value* data;
    llvm::Value* length;
    const auto receiverKind = expression.Receiver().Type().Kind();
    if (receiverKind == semantic::SemanticTypeKind::Slice ||
        receiverKind == semantic::SemanticTypeKind::OwnedBuffer ||
        receiverKind == semantic::SemanticTypeKind::OwnedSlice) {
        llvm::Value* source = EmitExpression(expression.Receiver());
        if (source == nullptr) return nullptr;
        data = _builder.CreateExtractValue(source, 0);
        length = _builder.CreateExtractValue(source, 1);
    } else if (receiverKind == semantic::SemanticTypeKind::Pointer) {
        data = EmitExpression(expression.Receiver());
        if (data == nullptr) return nullptr;
        const auto getCount = _module->getOrInsertFunction(
            "flecha_runtime_v1_allocation_count",
            llvm::FunctionType::get(sizeType, {data->getType()}, false));
        length = _builder.CreateCall(getCount, {data}, "allocation.count");
    } else {
        llvm::Value* storage = EmitArrayStorage(expression.Receiver());
        if (storage == nullptr) return nullptr;
        data = _builder.CreateInBoundsGEP(MapType(expression.Receiver().Type()),
                                          storage, {zero, zero});
        length = llvm::ConstantInt::get(sizeType,
                                        expression.Receiver().Type().Length());
    }
    llvm::Value* start = expression.Start() == nullptr
                             ? zero
                             : EmitExpression(*expression.Start());
    if (start == nullptr) return nullptr;
    llvm::Value* end = expression.End() == nullptr
                           ? length
                           : EmitExpression(*expression.End());
    if (end == nullptr) return nullptr;
    EmitBoundsCheck(_builder.CreateAnd(_builder.CreateICmpULE(start, end),
                                       _builder.CreateICmpULE(end, length)));
    data =
        receiverKind == semantic::SemanticTypeKind::Pointer
            ? _builder.CreateGEP(MapType(*expression.Type().Target()), data,
                                 start, "slice.data")
            : _builder.CreateInBoundsGEP(MapType(*expression.Type().Target()),
                                         data, start, "slice.data");
    llvm::Value* result =
        llvm::Constant::getNullValue(MapType(expression.Type()));
    result = _builder.CreateInsertValue(result, data, 0);
    return _builder.CreateInsertValue(result, _builder.CreateSub(end, start), 1,
                                      "slice");
}

llvm::Value* LlvmCodeGenerator::EmitAddress(
    const lowering::BoundPlaceExpression& place) {
    if (place.Kind() == lowering::BoundNodeKind::VariableExpression) {
        return _variables.at(
            &static_cast<const lowering::BoundVariableExpression&>(place)
                 .Symbol());
    }
    if (place.Kind() == lowering::BoundNodeKind::FieldExpression) {
        const auto& field =
            static_cast<const lowering::BoundFieldExpression&>(place);
        const auto type = field.Indirect() ? *field.Receiver().Type().Target()
                                           : field.Receiver().Type();
        llvm::Value* storage = field.Indirect()
                                   ? EmitExpression(field.Receiver())
                                   : EmitArrayStorage(field.Receiver());
        return storage
                   ? _builder.CreateStructGEP(MapType(type), storage,
                                              field.Index(), "field.address")
                   : nullptr;
    }
    if (place.Kind() == lowering::BoundNodeKind::ArrayElementExpression) {
        const auto& index =
            static_cast<const lowering::BoundArrayElementExpression&>(place);
        // Capture the receiver before evaluating the index, and finish the
        // bounds check before deriving an inbounds address or evaluating a
        // store RHS.
        const bool heap = index.Receiver().Type().Kind() ==
                          semantic::SemanticTypeKind::Pointer;
        const bool slice = index.Receiver().Type().Kind() ==
                               semantic::SemanticTypeKind::Slice ||
                           index.Receiver().Type().Kind() ==
                               semantic::SemanticTypeKind::OwnedBuffer ||
                           index.Receiver().Type().Kind() ==
                               semantic::SemanticTypeKind::OwnedSlice;
        llvm::Value* descriptor =
            (slice || heap) ? EmitExpression(index.Receiver()) : nullptr;
        if (slice && descriptor == nullptr) return nullptr;
        llvm::Value* storage = slice
                                   ? _builder.CreateExtractValue(descriptor, 0)
                               : heap ? descriptor
                                      : EmitArrayStorage(index.Receiver());
        if (storage == nullptr) return nullptr;
        llvm::Type* sizeType =
            MapType(semantic::SemanticType(semantic::SemanticTypeKind::USize));
        llvm::Value* length =
            slice ? _builder.CreateExtractValue(descriptor, 1)
                  : llvm::ConstantInt::get(sizeType,
                                           index.Receiver().Type().Length());
        if (heap) {
            const auto getCount = _module->getOrInsertFunction(
                "flecha_runtime_v1_allocation_count",
                llvm::FunctionType::get(sizeType, {storage->getType()}, false));
            length =
                _builder.CreateCall(getCount, {storage}, "allocation.count");
        }
        llvm::Value* offset =
            index.HasIndex()
                ? EmitExpression(index.Index())
                : (index.FromBack()
                       ? _builder.CreateSub(length,
                                            llvm::ConstantInt::get(sizeType, 1))
                       : llvm::ConstantInt::get(sizeType, 0));
        if (offset == nullptr) return nullptr;
        EmitBoundsCheck(_builder.CreateICmpULT(offset, length));
        if (slice || heap)
            return _builder.CreateInBoundsGEP(
                MapType(*index.Receiver().Type().Target()), storage, offset,
                "slice.element.address");
        return _builder.CreateInBoundsGEP(
            MapType(index.Receiver().Type()), storage,
            {llvm::ConstantInt::get(offset->getType(), 0), offset},
            "array.element.address");
    }
    return EmitExpression(
        static_cast<const lowering::BoundDereferenceExpression&>(place)
            .Pointer());
}

llvm::Value* LlvmCodeGenerator::EmitExpression(
    const lowering::BoundExpression& expression) {
    switch (expression.Kind()) {
        case lowering::BoundNodeKind::BuildExpression:
        case lowering::BoundNodeKind::DefaultExpression: {
            if (expression.Type().Kind() == semantic::SemanticTypeKind::Enum) {
                const auto value = expression.Type().Enum()->defaultValue;
                if (!value) {
                    Fail("missing enum default variant");
                    return nullptr;
                }
                auto* type = MapType(expression.Type());
                return llvm::ConstantInt::get(type, *value);
            }
            if (expression.Kind() ==
                    lowering::BoundNodeKind::DefaultExpression &&
                !expression.Type().ContainsStructure() &&
                !expression.Type().IsTriviallyZeroable()) {
                auto* storage = CreateTemporaryAlloca(
                    MapType(expression.Type()), "default.temporary");
                if (!EmitInto(expression, storage, expression.Type()))
                    return nullptr;
                return _builder.CreateLoad(MapType(expression.Type()), storage);
            }
            if (expression.Type().ContainsStructure()) {
                auto* storage = CreateOwnedValueTemporary(expression.Type(),
                                                          "build.temporary");
                EmitValueStorageCleanup(storage, expression.Type());
                if (!EmitInto(expression, storage, expression.Type()))
                    return nullptr;
                _builder.CreateStore(llvm::ConstantInt::getTrue(_context),
                                     LifetimeFlag(storage));
                return _builder.CreateLoad(MapType(expression.Type()), storage);
            }
            return llvm::Constant::getNullValue(MapType(expression.Type()));
        }

        case lowering::BoundNodeKind::SliceExpression:
            return EmitSlice(
                static_cast<const lowering::BoundSliceExpression&>(expression));
        case lowering::BoundNodeKind::TupleExpression: {
            const auto& tuple =
                static_cast<const lowering::BoundTupleExpression&>(expression);
            llvm::Value* result = llvm::PoisonValue::get(MapType(tuple.Type()));
            for (std::size_t i = 0; i < tuple.Elements().size(); ++i) {
                const auto& element = *tuple.Elements()[i];
                llvm::Value* field = EmitExpression(element);
                if (field == nullptr) return nullptr;
                field = CoerceValue(field, element.Type(),
                                    tuple.Type().Elements()[i]);
                if (field == nullptr) return nullptr;
                field = EmitValueCopy(field, tuple.Type().Elements()[i], true);
                result = _builder.CreateInsertValue(result, field, i);
            }
            return result;
        }
        case lowering::BoundNodeKind::ArrayExpression:
            return EmitArray(
                static_cast<const lowering::BoundArrayExpression&>(expression));
        case lowering::BoundNodeKind::MatchExpression:
            return EmitMatch(
                static_cast<const lowering::BoundMatchExpression&>(expression));
        case lowering::BoundNodeKind::ArrayLengthExpression: {
            const auto& length =
                static_cast<const lowering::BoundArrayLengthExpression&>(
                    expression);
            if (length.Receiver().Type().Kind() ==
                    semantic::SemanticTypeKind::Slice ||
                length.Receiver().Type().Kind() ==
                    semantic::SemanticTypeKind::OwnedBuffer ||
                length.Receiver().Type().Kind() ==
                    semantic::SemanticTypeKind::OwnedSlice) {
                llvm::Value* descriptor = EmitExpression(length.Receiver());
                return descriptor == nullptr ? nullptr
                                             : _builder.CreateExtractValue(
                                                   descriptor, 1, "slice.len");
            }
            if (EmitArrayStorage(length.Receiver()) == nullptr) return nullptr;
            return llvm::ConstantInt::get(MapType(expression.Type()),
                                          length.Receiver().Type().Length());
        }
        case lowering::BoundNodeKind::SliceDataExpression: {
            const auto& data =
                static_cast<const lowering::BoundSliceDataExpression&>(
                    expression);
            llvm::Value* descriptor = EmitExpression(data.Receiver());
            return descriptor == nullptr ? nullptr
                                         : _builder.CreateExtractValue(
                                               descriptor, 0, "slice.data");
        }
        case lowering::BoundNodeKind::AddressExpression:
            return EmitAddress(
                static_cast<const lowering::BoundAddressExpression&>(expression)
                    .Place());
        case lowering::BoundNodeKind::FieldExpression:
        case lowering::BoundNodeKind::ArrayElementExpression:
        case lowering::BoundNodeKind::DereferenceExpression: {
            llvm::Value* address = EmitAddress(
                static_cast<const lowering::BoundPlaceExpression&>(expression));
            return address == nullptr
                       ? nullptr
                       : _builder.CreateLoad(MapType(expression.Type()),
                                             address, "deref");
        }
        case lowering::BoundNodeKind::LiteralExpression:
            return EmitLiteral(
                static_cast<const lowering::BoundLiteralExpression&>(
                    expression));
        case lowering::BoundNodeKind::DirectExpression: {
            const auto& direct =
                static_cast<const lowering::BoundDirectExpression&>(expression);
            auto* storage = CreateOwnedValueTemporary(expression.Type(),
                                                      "direct.temporary");
            if (!EmitInto(direct.Value(), storage, expression.Type()))
                return nullptr;
            if (expression.Type().ContainsStructure())
                _builder.CreateStore(llvm::ConstantInt::getTrue(_context),
                                     LifetimeFlag(storage));
            llvm::Value* value = _builder.CreateLoad(MapType(expression.Type()),
                                                     storage, "direct.value");
            if (expression.Type().ContainsStructure())
                _builder.CreateStore(llvm::ConstantInt::getFalse(_context),
                                     LifetimeFlag(storage));
            if (!expression.Type().ContainsStructure() &&
                (expression.Type().OwnsStorage() ||
                 expression.Type().ContainsOwnedBuffer()) &&
                !expression.Type().IsCopyable())
                _builder.CreateStore(
                    llvm::Constant::getNullValue(MapType(expression.Type())),
                    storage);
            return value;
        }
        case lowering::BoundNodeKind::MoveExpression: {
            const auto& move =
                static_cast<const lowering::BoundMoveExpression&>(expression);
            llvm::Value* storage = EmitAddress(move.Source());
            if (storage == nullptr) return nullptr;
            llvm::Value* value = _builder.CreateLoad(MapType(expression.Type()),
                                                     storage, "moved.value");
            if (expression.Type().ContainsStructure())
                _builder.CreateStore(llvm::ConstantInt::getFalse(_context),
                                     LifetimeFlag(storage));
            if (!expression.Type().ContainsStructure() &&
                (((expression.Type().OwnsStorage() ||
                   expression.Type().ContainsOwnedBuffer()) &&
                  !expression.Type().IsCopyable()) ||
                 _ownedCaptureStorage.contains(storage)))
                _builder.CreateStore(
                    llvm::Constant::getNullValue(MapType(expression.Type())),
                    storage);
            return value;
        }
        case lowering::BoundNodeKind::VariableExpression: {
            const semantic::VariableSymbol& symbol =
                static_cast<const lowering::BoundVariableExpression&>(
                    expression)
                    .Symbol();
            llvm::Value* storage = _variables.at(&symbol);
            return _builder.CreateLoad(MapType(symbol.Type()), storage,
                                       symbol.Name());
        }
        case lowering::BoundNodeKind::IncrementExpression: {
            const auto& increment =
                static_cast<const lowering::BoundIncrementExpression&>(
                    expression);
            llvm::Value* storage = EmitAddress(increment.Target());
            if (storage == nullptr) return nullptr;
            llvm::Type* type = MapType(expression.Type());
            llvm::Value* previous =
                _builder.CreateLoad(type, storage, "increment.old");
            llvm::Value* next = _builder.CreateAdd(
                previous, llvm::ConstantInt::get(type, 1), "increment.next");
            _builder.CreateStore(next, storage);
            return previous;
        }
        case lowering::BoundNodeKind::UnaryExpression:
            return EmitUnary(
                static_cast<const lowering::BoundUnaryExpression&>(expression));
        case lowering::BoundNodeKind::CastExpression:
            return EmitCast(
                static_cast<const lowering::BoundCastExpression&>(expression));
        case lowering::BoundNodeKind::BinaryExpression:
            return EmitBinary(
                static_cast<const lowering::BoundBinaryExpression&>(
                    expression));
        case lowering::BoundNodeKind::IntegerLimitExpression:
            return EmitIntegerLimit(
                static_cast<const lowering::BoundIntegerLimitExpression&>(
                    expression));
        case lowering::BoundNodeKind::FloatingConstantExpression:
            return EmitFloatingConstant(
                static_cast<const lowering::BoundFloatingConstantExpression&>(
                    expression));
        case lowering::BoundNodeKind::OverflowingArithmeticExpression:
            return EmitOverflowingArithmetic(
                static_cast<
                    const lowering::BoundOverflowingArithmeticExpression&>(
                    expression));
        case lowering::BoundNodeKind::AssignmentExpression: {
            const lowering::BoundAssignmentExpression& assignment =
                static_cast<const lowering::BoundAssignmentExpression&>(
                    expression);
            llvm::Value* storage = EmitAddress(assignment.Target());
            if (storage == nullptr) return nullptr;
            llvm::Value* value = EmitExpression(assignment.Value());
            if (value == nullptr) return nullptr;
            value = CoerceValue(value, assignment.Value().Type(),
                                assignment.Target().Type());
            if (value != nullptr && assignment.Target().Type().OwnsStorage()) {
                value = EmitValueCopy(value, assignment.Target().Type(), false);
            }
            if (value == nullptr) return nullptr;
            if (assignment.Target().Type().OwnsStorage()) {
                if (assignment.Target().Kind() ==
                    lowering::BoundNodeKind::VariableExpression)
                    EmitValueStorageCleanup(storage,
                                            assignment.Target().Type());
                else
                    EmitValueDestroy(
                        _builder.CreateLoad(MapType(assignment.Target().Type()),
                                            storage, "value.old"),
                        assignment.Target().Type());
            }
            _builder.CreateStore(value, storage);
            if (assignment.Target().Type().ContainsStructure() &&
                assignment.Target().Kind() ==
                    lowering::BoundNodeKind::VariableExpression)
                _builder.CreateStore(llvm::ConstantInt::getTrue(_context),
                                     LifetimeFlag(storage));
            return value;
        }
        case lowering::BoundNodeKind::AllocationExpression:
            return EmitAllocation(
                static_cast<const lowering::BoundAllocationExpression&>(
                    expression));
        case lowering::BoundNodeKind::CallExpression: {
            const lowering::BoundCallExpression& call =
                static_cast<const lowering::BoundCallExpression&>(expression);
            if (call.Function().BuiltIn() == semantic::BuiltInFunction::Take) {
                auto* address = EmitAddress(
                    static_cast<const lowering::BoundPlaceExpression&>(
                        *call.Arguments()[0]));
                if (!address) return nullptr;
                auto* value = _builder.CreateLoad(MapType(call.Type()), address,
                                                  "taken.owner");
                _builder.CreateStore(
                    llvm::Constant::getNullValue(MapType(call.Type())),
                    address);
                return value;
            }
            if (call.Type().ContainsStructure()) {
                auto* storage = CreateOwnedValueTemporary(
                    call.Type(), "structure.call.result");
                EmitValueStorageCleanup(storage, call.Type());
                if (!EmitInto(call, storage, call.Type())) return nullptr;
                _builder.CreateStore(llvm::ConstantInt::getTrue(_context),
                                     LifetimeFlag(storage));
                return _builder.CreateLoad(MapType(call.Type()), storage);
            }
            std::vector<llvm::Value*> arguments;
            arguments.reserve(call.Arguments().size());
            std::size_t argumentIndex = 0;
            for (const std::unique_ptr<lowering::BoundExpression>& argument :
                 call.Arguments()) {
                llvm::Value* value = EmitExpression(*argument);
                if (value == nullptr) return nullptr;
                if (call.Function().BuiltIn() ==
                        semantic::BuiltInFunction::None &&
                    argumentIndex < call.Function().ParameterTypes().size()) {
                    value = CoerceValue(
                        value, argument->Type(),
                        call.Function().ParameterTypes()[argumentIndex]);
                    if (value == nullptr) return nullptr;
                }
                if (call.Function().BuiltIn() ==
                    semantic::BuiltInFunction::None)
                    value = EmitValueCopy(
                        value, call.Function().ParameterTypes()[argumentIndex],
                        true);
                arguments.push_back(value);
                ++argumentIndex;
            }
            if (call.Function().BuiltIn() ==
                    semantic::BuiltInFunction::Stdout ||
                call.Function().BuiltIn() ==
                    semantic::BuiltInFunction::StdoutLine ||
                call.Function().BuiltIn() ==
                    semantic::BuiltInFunction::StdoutFormat)
                return EmitBuiltInOutput(call, arguments);
            if (call.Function().BuiltIn() == semantic::BuiltInFunction::Free)
                return EmitFree(arguments.front(),
                                call.Function().ParameterTypes()[0]);
            llvm::Function* function = _functions.at(&call.Function());
            llvm::AllocaInst* ownedResult = nullptr;
            if (call.Type().OwnsStorage()) {
                ownedResult =
                    CreateOwnedValueTemporary(call.Type(), "owned.call.result");
                EmitValueStorageCleanup(ownedResult, call.Type());
            }
            llvm::Value* result = EmitFunctionCall(
                call.Function(), arguments,
                function->getReturnType()->isVoidTy() ? "" : "call");
            if (ownedResult == nullptr) return result;
            _builder.CreateStore(result, ownedResult);
            llvm::Value* owned = _builder.CreateLoad(MapType(call.Type()),
                                                     ownedResult, "owned.call");
            if (!call.Type().IsCopyable())
                _freshOwnedValues.emplace(owned, ownedResult);
            return owned;
        }
        case lowering::BoundNodeKind::IndirectCallExpression: {
            const auto& call =
                static_cast<const lowering::BoundIndirectCallExpression&>(
                    expression);
            if (call.Type().ContainsStructure()) {
                auto* storage = CreateOwnedValueTemporary(
                    call.Type(), "indirect.call.result");
                EmitValueStorageCleanup(storage, call.Type());
                if (!EmitIndirectCall(call, storage)) return nullptr;
                _builder.CreateStore(llvm::ConstantInt::getTrue(_context),
                                     LifetimeFlag(storage));
                return _builder.CreateLoad(MapType(call.Type()), storage);
            }
            llvm::Value* result = EmitIndirectCall(call);
            if (!result || !call.Type().OwnsStorage()) return result;
            auto* storage =
                CreateOwnedValueTemporary(call.Type(), "indirect.call.owner");
            EmitValueStorageCleanup(storage, call.Type());
            _builder.CreateStore(result, storage);
            llvm::Value* owned = _builder.CreateLoad(
                MapType(call.Type()), storage, "indirect.call.owner.value");
            if (!call.Type().IsCopyable())
                _freshOwnedValues.emplace(owned, storage);
            return owned;
        }
        case lowering::BoundNodeKind::StringViewOperationExpression:
            return EmitStringViewOperation(
                static_cast<
                    const lowering::BoundStringViewOperationExpression&>(
                    expression));
        case lowering::BoundNodeKind::FunctionExpression: {
            return EmitClosure(
                static_cast<const lowering::BoundFunctionExpression&>(
                    expression));
        }
        default:
            Fail("unsupported bound expression");
            return nullptr;
    }
}

llvm::Value* LlvmCodeGenerator::EmitMatch(
    const lowering::BoundMatchExpression& expression,
    llvm::Value* destination) {
    llvm::Value* matched = EmitExpression(expression.Matched());
    if (matched == nullptr) return nullptr;

    const bool returnsVoid =
        expression.Type().Kind() == semantic::SemanticTypeKind::Void;
    llvm::Value* result = destination;
    const bool ownsResult =
        result == nullptr && expression.Type().OwnsStorage();
    if (!returnsVoid) {
        if (result == nullptr)
            result = ownsResult
                         ? CreateOwnedValueTemporary(expression.Type(),
                                                     "match.result")
                         : CreateTemporaryAlloca(MapType(expression.Type()),
                                                 "match.result");
    }

    llvm::BasicBlock* merge =
        llvm::BasicBlock::Create(_context, "match.end", _currentFunction);
    std::vector<llvm::BasicBlock*> armBlocks;
    armBlocks.reserve(expression.Arms().size());
    llvm::BasicBlock* wildcard = nullptr;
    for (const auto& arm : expression.Arms()) {
        llvm::BasicBlock* block =
            llvm::BasicBlock::Create(_context, "match.arm", _currentFunction);
        armBlocks.push_back(block);
        if (!arm.discriminant.has_value()) wildcard = block;
    }
    llvm::BasicBlock* invalid =
        wildcard == nullptr ? llvm::BasicBlock::Create(
                                  _context, "match.invalid", _currentFunction)
                            : nullptr;
    llvm::SwitchInst* dispatch = _builder.CreateSwitch(
        matched, wildcard == nullptr ? invalid : wildcard,
        expression.Arms().size() - (wildcard == nullptr ? 0 : 1));
    auto* matchedType = llvm::cast<llvm::IntegerType>(matched->getType());
    for (std::size_t index = 0; index < expression.Arms().size(); ++index) {
        const auto& arm = expression.Arms()[index];
        if (arm.discriminant.has_value())
            dispatch->addCase(
                llvm::ConstantInt::get(matchedType, *arm.discriminant),
                armBlocks[index]);
    }

    for (std::size_t index = 0; index < expression.Arms().size(); ++index) {
        _builder.SetInsertPoint(armBlocks[index]);
        if (returnsVoid) {
            if (EmitExpression(*expression.Arms()[index].value) == nullptr)
                return nullptr;
        } else if (!EmitInto(*expression.Arms()[index].value, result,
                             expression.Type())) {
            return nullptr;
        }
        if (TerminatorOrNull(_builder.GetInsertBlock()) == nullptr)
            _builder.CreateBr(merge);
    }

    if (invalid != nullptr) {
        _builder.SetInsertPoint(invalid);
        _builder.CreateUnreachable();
    }
    _builder.SetInsertPoint(merge);
    if (returnsVoid) return llvm::ConstantInt::getFalse(_context);
    if (ownsResult && expression.Type().ContainsStructure())
        _builder.CreateStore(llvm::ConstantInt::getTrue(_context),
                             LifetimeFlag(result));
    return _builder.CreateLoad(MapType(expression.Type()), result,
                               "match.value");
}

llvm::Value* LlvmCodeGenerator::LifetimeFlag(llvm::Value* storage) {
    const auto found = _lifetimeFlags.find(storage);
    if (found != _lifetimeFlags.end()) return found->second;
    auto* flag = CreateEntryStorage(llvm::Type::getInt1Ty(_context),
                                    "structure.live", true);
    _lifetimeFlags[storage] = flag;
    return flag;
}

void LlvmCodeGenerator::EmitArrayElements(
    llvm::Value* storage, semantic::SemanticType type, bool reverse,
    const std::function<void(llvm::Value*)>& action) {
    auto* countType = llvm::Type::getInt64Ty(_context);
    auto* before = _builder.GetInsertBlock();
    auto* check =
        llvm::BasicBlock::Create(_context, "fields.check", _currentFunction);
    auto* body =
        llvm::BasicBlock::Create(_context, "fields.body", _currentFunction);
    auto* done =
        llvm::BasicBlock::Create(_context, "fields.done", _currentFunction);
    _builder.CreateBr(check);
    _builder.SetInsertPoint(check);
    auto* index = _builder.CreatePHI(countType, 2, "fields.index");
    index->addIncoming(
        llvm::ConstantInt::get(countType, reverse ? type.Length() : 0), before);
    llvm::Value* valid =
        reverse
            ? _builder.CreateICmpNE(index, llvm::ConstantInt::get(countType, 0))
            : _builder.CreateICmpULT(
                  index, llvm::ConstantInt::get(countType, type.Length()));
    _builder.CreateCondBr(valid, body, done);
    _builder.SetInsertPoint(body);
    llvm::Value* offset =
        reverse
            ? _builder.CreateSub(index, llvm::ConstantInt::get(countType, 1))
            : index;
    action(_builder.CreateInBoundsGEP(
        MapType(type), storage,
        {llvm::ConstantInt::get(countType, 0), offset}));
    llvm::Value* next =
        reverse
            ? offset
            : _builder.CreateAdd(index, llvm::ConstantInt::get(countType, 1));
    index->addIncoming(next, _builder.GetInsertBlock());
    _builder.CreateBr(check);
    _builder.SetInsertPoint(done);
}

void LlvmCodeGenerator::EmitDestroyAt(llvm::Value* storage,
                                      semantic::SemanticType type) {
    using Kind = semantic::SemanticTypeKind;
    if (type.Kind() == Kind::Structure) {
        if (type.Structure()->tear)
            _builder.CreateCall(_functions.at(type.Structure()->tear),
                                {storage});
        const auto& fields = type.Structure()->fields;
        for (std::size_t i = fields.size(); i > 0; --i)
            if (fields[i - 1].type.OwnsStorage())
                EmitDestroyAt(
                    _builder.CreateStructGEP(MapType(type), storage, i - 1),
                    fields[i - 1].type);
    } else if (type.Kind() == Kind::Tuple) {
        for (std::size_t i = type.Elements().size(); i > 0; --i)
            if (type.Elements()[i - 1].OwnsStorage())
                EmitDestroyAt(
                    _builder.CreateStructGEP(MapType(type), storage, i - 1),
                    type.Elements()[i - 1]);
    } else if (type.Kind() == Kind::Array && type.Target()->OwnsStorage()) {
        EmitArrayElements(storage, type, true, [&](llvm::Value* slot) {
            EmitDestroyAt(slot, *type.Target());
        });
    } else if (type.Kind() == Kind::String)
        EmitStringDestroy(_builder.CreateLoad(MapType(type), storage));
    else if (type.Kind() == Kind::OwnedSlice)
        EmitFree(_builder.CreateLoad(MapType(type), storage), type);
    else if (type.Kind() == Kind::Function)
        EmitValueDestroy(_builder.CreateLoad(MapType(type), storage), type);
}

bool LlvmCodeGenerator::EmitDefaultInto(llvm::Value* destination,
                                        semantic::SemanticType type,
                                        bool semanticZero) {
    using Kind = semantic::SemanticTypeKind;
    if (type.Kind() == Kind::Enum) {
        if (!type.Enum()->defaultValue)
            return Fail("missing enum default variant");
        _builder.CreateStore(
            llvm::ConstantInt::get(MapType(type), *type.Enum()->defaultValue),
            destination);
    } else if (type.Kind() == Kind::Structure) {
        const semantic::FunctionSymbol* constructor =
            semanticZero ? type.Structure()->zero : type.Structure()->init;
        if (!constructor)
            return Fail(semanticZero ? "missing structure default constructor"
                                     : "missing structure initializer");
        if (!semanticZero && !constructor->ParameterTypes().empty())
            return Fail(
                "default construction requires a zero-argument initializer");
        _builder.CreateCall(_functions.at(constructor), {destination});
    } else if (type.Kind() == Kind::Array &&
               (type.ContainsStructure() || !type.IsTriviallyZeroable())) {
        bool success = true;
        EmitArrayElements(destination, type, false, [&](llvm::Value* slot) {
            success &= EmitDefaultInto(slot, *type.Target(), semanticZero);
        });
        return success;
    } else if (type.Kind() == Kind::Tuple &&
               (type.ContainsStructure() || !type.IsTriviallyZeroable())) {
        for (std::size_t i = 0; i < type.Elements().size(); ++i)
            if (!EmitDefaultInto(
                    _builder.CreateStructGEP(MapType(type), destination, i),
                    type.Elements()[i], semanticZero))
                return false;
    } else
        _builder.CreateStore(llvm::Constant::getNullValue(MapType(type)),
                             destination);
    return true;
}

bool LlvmCodeGenerator::EmitInto(const lowering::BoundExpression& expression,
                                 llvm::Value* destination,
                                 semantic::SemanticType type) {
    using Node = lowering::BoundNodeKind;
    using Kind = semantic::SemanticTypeKind;
    if (expression.Kind() == Node::DirectExpression)
        return EmitInto(
            static_cast<const lowering::BoundDirectExpression&>(expression)
                .Value(),
            destination, type);
    if (expression.Kind() == Node::DefaultExpression)
        return EmitDefaultInto(
            destination, type,
            static_cast<const lowering::BoundDefaultExpression&>(expression)
                .IsSemanticZero());
    if (expression.Kind() == Node::BuildExpression) {
        const auto& build =
            static_cast<const lowering::BoundBuildExpression&>(expression);
        for (std::size_t i = 0; i < build.Defaults().size(); ++i) {
            llvm::Value* field =
                _builder.CreateStructGEP(MapType(type), destination, i);
            if (!EmitInto(*build.Defaults()[i], field,
                          type.Structure()->fields[i].type))
                return false;
        }
        _variables[&build.Self()] = destination;
        DeclarePositions(build.Body());
        std::vector<const lowering::BoundBlockStatement*> path;
        for (const auto& scope : _cleanupScopes)
            if (scope->block) path.push_back(scope->block);
        CollectPositionScopePaths(build.Body(), path);
        if (!EmitBlock(build.Body())) return false;
        return true;
    }
    if (expression.Kind() == Node::CallExpression && type.ContainsStructure()) {
        const auto& call =
            static_cast<const lowering::BoundCallExpression&>(expression);
        std::vector<llvm::Value*> arguments{destination};
        for (std::size_t i = 0; i < call.Arguments().size(); ++i) {
            llvm::Value* argument = EmitExpression(*call.Arguments()[i]);
            if (!argument) return false;
            argument = CoerceValue(argument, call.Arguments()[i]->Type(),
                                   call.Function().ParameterTypes()[i]);
            if (!argument) return false;
            arguments.push_back(argument);
        }
        EmitFunctionCall(call.Function(), arguments);
        return true;
    }
    if (expression.Kind() == Node::IndirectCallExpression &&
        type.ContainsStructure())
        return EmitIndirectCall(
                   static_cast<const lowering::BoundIndirectCallExpression&>(
                       expression),
                   destination) != nullptr;
    if (expression.Kind() == Node::MatchExpression && type.ContainsStructure())
        return EmitMatch(static_cast<const lowering::BoundMatchExpression&>(
                             expression),
                         destination) != nullptr;
    if (expression.Kind() == Node::TupleExpression &&
        type.ContainsStructure()) {
        const auto& tuple =
            static_cast<const lowering::BoundTupleExpression&>(expression);
        for (std::size_t i = 0; i < tuple.Elements().size(); ++i)
            if (!EmitInto(
                    *tuple.Elements()[i],
                    _builder.CreateStructGEP(MapType(type), destination, i),
                    type.Elements()[i]))
                return false;
        return true;
    }
    if (expression.Kind() == Node::ArrayExpression &&
        type.ContainsStructure()) {
        const auto& array =
            static_cast<const lowering::BoundArrayExpression&>(expression);
        if (array.IsRepeat() && !type.Target()->IsCopyable() &&
            type.Length() == 0) {
            llvm::Value* value = EmitExpression(*array.Elements()[0]);
            if (!value) return false;
            EmitValueDestroy(value, *type.Target());
            return true;
        }
        if (array.IsRepeat() && !type.Target()->IsCopyable() &&
            type.Length() == 1) {
            auto* indexType = llvm::Type::getInt64Ty(_context);
            auto* zero = llvm::ConstantInt::get(indexType, 0);
            return EmitInto(*array.Elements()[0],
                            _builder.CreateInBoundsGEP(
                                MapType(type), destination, {zero, zero}),
                            *type.Target());
        }
        if (array.IsRepeat()) {
            // Only copyable elements may repeat; evaluate the source once.
            llvm::Value* value = EmitExpression(*array.Elements()[0]);
            if (!value) return false;
            EmitArrayElements(destination, type, false, [&](llvm::Value* slot) {
                llvm::Value* copy = EmitValueCopy(value, *type.Target(), false);
                if (copy) _builder.CreateStore(copy, slot);
            });
        } else
            for (std::size_t i = 0; i < array.Elements().size(); ++i) {
                auto* indexType = llvm::Type::getInt64Ty(_context);
                llvm::Value* slot = _builder.CreateInBoundsGEP(
                    MapType(type), destination,
                    {llvm::ConstantInt::get(indexType, 0),
                     llvm::ConstantInt::get(indexType, i)});
                if (!EmitInto(*array.Elements()[i], slot, *type.Target()))
                    return false;
            }
        return true;
    }
    llvm::Value* value = EmitExpression(expression);
    if (!value) return false;
    value = CoerceValue(value, expression.Type(), type);
    if (value && type.OwnsStorage()) value = EmitValueCopy(value, type, false);
    if (!value) return false;
    _builder.CreateStore(value, destination);
    return true;
}

llvm::Value* LlvmCodeGenerator::EmitAllocation(
    const lowering::BoundAllocationExpression& expression) {
    llvm::Type* element = MapType(expression.ElementType());
    const llvm::DataLayout& layout = _module->getDataLayout();
    llvm::Type* sizeType =
        MapType(semantic::SemanticType(semantic::SemanticTypeKind::USize));
    llvm::Type* pointerType = llvm::PointerType::getUnqual(_context);
    const bool dynamic = expression.IsDynamic();
    llvm::Value* count =
        dynamic ? EmitExpression(expression.Initializer()) : nullptr;
    if (dynamic && !count) return nullptr;
    std::vector<llvm::Type*> parameters = {sizeType, sizeType};
    std::vector<llvm::Value*> arguments = {
        llvm::ConstantInt::get(
            sizeType, layout.getTypeAllocSize(element).getFixedValue()),
        llvm::ConstantInt::get(sizeType,
                               layout.getABITypeAlign(element).value())};
    if (dynamic) {
        parameters.insert(parameters.begin(), sizeType);
        arguments.insert(arguments.begin(), count);
    }
    llvm::FunctionCallee allocate = _module->getOrInsertFunction(
        dynamic ? "flecha_runtime_v1_alloc_array" : "flecha_runtime_v1_alloc",
        llvm::FunctionType::get(pointerType, parameters, false));
    llvm::Value* pointer =
        _builder.CreateCall(allocate, arguments, "allocation");
    auto* success = llvm::BasicBlock::Create(_context, "allocation.success",
                                             _currentFunction);
    auto* failure = llvm::BasicBlock::Create(_context, "allocation.failure",
                                             _currentFunction);
    auto* done =
        llvm::BasicBlock::Create(_context, "allocation.done", _currentFunction);
    _builder.CreateCondBr(
        _builder.CreateICmpNE(pointer,
                              llvm::Constant::getNullValue(pointerType)),
        success, failure);
    _builder.SetInsertPoint(success);
    if (!dynamic) {
        if (!EmitInto(expression.Initializer(), pointer,
                      expression.ElementType()))
            return nullptr;
    } else if (expression.ElementType().Kind() ==
                   semantic::SemanticTypeKind::Structure ||
               (expression.ElementType().Kind() ==
                    semantic::SemanticTypeKind::Enum &&
                !expression.ElementType().IsTriviallyZeroable())) {
        auto* indexStorage =
            CreateTemporaryAlloca(sizeType, "allocation.zero.index");
        _builder.CreateStore(llvm::ConstantInt::get(sizeType, 0), indexStorage);
        auto* condition = llvm::BasicBlock::Create(
            _context, "allocation.zero.condition", _currentFunction);
        auto* body = llvm::BasicBlock::Create(_context, "allocation.zero.body",
                                              _currentFunction);
        auto* initialized = llvm::BasicBlock::Create(
            _context, "allocation.zero.done", _currentFunction);
        _builder.CreateBr(condition);
        _builder.SetInsertPoint(condition);
        llvm::Value* index =
            _builder.CreateLoad(sizeType, indexStorage, "allocation.zero.i");
        _builder.CreateCondBr(_builder.CreateICmpULT(index, count), body,
                              initialized);
        _builder.SetInsertPoint(body);
        llvm::Value* slot = _builder.CreateInBoundsGEP(element, pointer, index,
                                                       "allocation.zero.slot");
        if (!EmitDefaultInto(slot, expression.ElementType(), true))
            return nullptr;
        _builder.CreateStore(
            _builder.CreateAdd(index, llvm::ConstantInt::get(sizeType, 1)),
            indexStorage);
        _builder.CreateBr(condition);
        _builder.SetInsertPoint(initialized);
    }
    llvm::Value* successful =
        llvm::Constant::getNullValue(MapType(expression.Type()));
    if (dynamic) {
        llvm::Value* owner = llvm::Constant::getNullValue(
            MapType(expression.Type().Elements()[0]));
        owner = _builder.CreateInsertValue(owner, pointer, 0);
        owner = _builder.CreateInsertValue(owner, count, 1, "allocation.owner");
        successful = _builder.CreateInsertValue(successful, owner, 0);
    } else {
        successful = _builder.CreateInsertValue(successful, pointer, 0);
    }
    llvm::BasicBlock* successEnd = _builder.GetInsertBlock();
    _builder.CreateBr(done);
    _builder.SetInsertPoint(failure);
    llvm::Value* error = CoerceValue(
        llvm::ConstantInt::get(MapType(expression.FailureType()), 1),
        expression.FailureType(),
        semantic::SemanticType(semantic::SemanticTypeKind::ErrorValue));
    llvm::Value* failed = _builder.CreateInsertValue(
        llvm::Constant::getNullValue(MapType(expression.Type())), error, 1);
    _builder.CreateBr(done);
    _builder.SetInsertPoint(done);
    auto* result =
        _builder.CreatePHI(MapType(expression.Type()), 2, "allocation.result");
    result->addIncoming(successful, successEnd);
    result->addIncoming(failed, failure);
    return result;
}

llvm::Value* LlvmCodeGenerator::EmitFree(llvm::Value* owner,
                                         semantic::SemanticType ownerType) {
    llvm::Value* pointer =
        (ownerType.Kind() == semantic::SemanticTypeKind::OwnedBuffer ||
         ownerType.Kind() == semantic::SemanticTypeKind::OwnedSlice)
            ? _builder.CreateExtractValue(owner, 0, "free.data")
            : owner;
    const semantic::SemanticType target = *ownerType.Target();
    auto* release =
        llvm::BasicBlock::Create(_context, "free.release", _currentFunction);
    auto* done =
        llvm::BasicBlock::Create(_context, "free.done", _currentFunction);
    _builder.CreateCondBr(
        _builder.CreateICmpNE(pointer,
                              llvm::Constant::getNullValue(pointer->getType())),
        release, done);
    _builder.SetInsertPoint(release);
    EmitDestroyAt(pointer, target);
    llvm::FunctionCallee deallocate = _module->getOrInsertFunction(
        "flecha_runtime_v1_free",
        llvm::FunctionType::get(llvm::Type::getVoidTy(_context),
                                {pointer->getType()}, false));
    _builder.CreateCall(deallocate, {pointer});
    _builder.CreateBr(done);
    _builder.SetInsertPoint(done);
    // Expression statements need a non-null IR result even for void built-ins.
    return llvm::UndefValue::get(llvm::Type::getInt1Ty(_context));
}

std::unique_ptr<llvm::Module> LlvmCodeGenerator::Generate(
    const lowering::BoundProgram& program, std::string moduleName) {
    _error.clear();
    _functions.clear();
    _callableAdapters.clear();
    _boundFunctions.clear();
    _closureEnvironmentTypes.clear();
    _closureDestroyers.clear();
    _structureTypes.clear();
    _variables.clear();
    _module = std::make_unique<llvm::Module>(moduleName, _context);
    if (!_targetTriple.empty()) {
#if LLVM_VERSION_MAJOR >= 21
        _module->setTargetTriple(llvm::Triple(_targetTriple));
#else
        _module->setTargetTriple(_targetTriple);
#endif
    }
    if (_dataLayout.has_value()) _module->setDataLayout(*_dataLayout);
    if (!DeclareFunctions(program)) return nullptr;
    for (const std::unique_ptr<lowering::BoundFunction>& function :
         program.Functions()) {
        if (function->IsExtern()) continue;
        if (!EmitFunction(*function)) return nullptr;
    }
    const std::string verificationError = VerifyModule(*_module);
    if (!verificationError.empty()) {
        std::string generatedModule;
        llvm::raw_string_ostream output(generatedModule);
        _module->print(output, nullptr);
        output.flush();
        Fail("LLVM module verification failed: " + verificationError +
             "generated module:\n" + generatedModule);
        return nullptr;
    }
    return std::move(_module);
}

std::string VerifyModule(const llvm::Module& module) {
    std::string message;
    llvm::raw_string_ostream output(message);
    if (!llvm::verifyModule(module, &output)) return {};
    output.flush();
    return message;
}

}  // namespace flecha::codegen
