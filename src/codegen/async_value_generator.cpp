#include "codegen/async_value_generator.hpp"

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Linker/Linker.h>
#include <llvm/Support/raw_ostream.h>

#include <algorithm>
#include <charconv>
#include <map>
#include <set>
#include <unordered_map>

#include "codegen/async_code_generator.hpp"
#include "core/ast_walk.hpp"
#include "runtime/async_executor.hpp"

namespace flecha::codegen {
namespace {
using semantic::AsyncSlotKind;
using semantic::AsyncStateKind;
using semantic::SemanticType;
using semantic::SemanticTypeKind;

std::size_t Align(std::size_t value, std::size_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

bool SignedInteger(SemanticType type) {
    return type.Kind() == SemanticTypeKind::I8 ||
           type.Kind() == SemanticTypeKind::I16 ||
           type.Kind() == SemanticTypeKind::I32 ||
           type.Kind() == SemanticTypeKind::I64 ||
           type.Kind() == SemanticTypeKind::ISize;
}

const core::Expression& Unwrap(const core::Expression& expression) {
    if (expression.Kind() == core::AstNodeKind::ParenthesizedExpression)
        return Unwrap(
            static_cast<const core::ParenthesizedExpression&>(expression)
                .Inner());
    return expression;
}

class TypeMapper final {
   public:
    TypeMapper(llvm::LLVMContext& context, const llvm::DataLayout& layout)
        : _context(context), _layout(layout) {}

    llvm::Type* Map(SemanticType type) {
        switch (type.Kind()) {
            case SemanticTypeKind::Enum:
                return Map(type.Enum()->backing);
            case SemanticTypeKind::Structure: {
                const auto found = _structures.find(type.Structure());
                if (found != _structures.end()) return found->second;
                auto* result = llvm::StructType::create(
                    _context, "flecha.async.value." + type.Structure()->name);
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
            case SemanticTypeKind::ErrorValue:
                return llvm::StructType::get(
                    _context, {llvm::PointerType::getUnqual(_context),
                               llvm::Type::getInt32Ty(_context)});
            case SemanticTypeKind::NamedError:
                return llvm::Type::getInt32Ty(_context);
            case SemanticTypeKind::Tuple: {
                std::vector<llvm::Type*> fields;
                for (SemanticType element : type.Elements()) {
                    llvm::Type* mapped = Map(element);
                    if (!mapped) return nullptr;
                    fields.push_back(mapped);
                }
                return llvm::StructType::get(_context, fields);
            }
            case SemanticTypeKind::Slice:
            case SemanticTypeKind::OwnedBuffer: {
                llvm::Type* size = llvm::IntegerType::get(
                    _context, _layout.getPointerSizeInBits());
                return llvm::StructType::get(
                    _context, {llvm::PointerType::getUnqual(_context), size});
            }
            case SemanticTypeKind::OwnedSlice: {
                llvm::Type* size = llvm::IntegerType::get(
                    _context, _layout.getPointerSizeInBits());
                return llvm::StructType::get(
                    _context,
                    {llvm::PointerType::getUnqual(_context), size, size});
            }
            case SemanticTypeKind::Array: {
                llvm::Type* element = Map(*type.Target());
                return element ? llvm::ArrayType::get(element, type.Length())
                               : nullptr;
            }
            case SemanticTypeKind::I8:
            case SemanticTypeKind::U8:
            case SemanticTypeKind::Char:
                return llvm::Type::getInt8Ty(_context);
            case SemanticTypeKind::I16:
            case SemanticTypeKind::U16:
                return llvm::Type::getInt16Ty(_context);
            case SemanticTypeKind::I32:
            case SemanticTypeKind::U32:
                return llvm::Type::getInt32Ty(_context);
            case SemanticTypeKind::I64:
            case SemanticTypeKind::U64:
                return llvm::Type::getInt64Ty(_context);
            case SemanticTypeKind::ISize:
            case SemanticTypeKind::USize:
                return llvm::IntegerType::get(_context,
                                              _layout.getPointerSizeInBits());
            case SemanticTypeKind::F32:
                return llvm::Type::getFloatTy(_context);
            case SemanticTypeKind::F64:
                return llvm::Type::getDoubleTy(_context);
            case SemanticTypeKind::Bool:
                return llvm::Type::getInt1Ty(_context);
            case SemanticTypeKind::Reference:
            case SemanticTypeKind::Pointer:
            case SemanticTypeKind::Null:
            case SemanticTypeKind::Task:
                return llvm::PointerType::getUnqual(_context);
            case SemanticTypeKind::Void:
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

struct FunctionEntry final {
    const semantic::FunctionSymbol* symbol;
    const semantic::AsyncFramePlan* plan;
    lowering::AsyncMachine machine;
    GeneratedAsyncFunction generated;
};

struct DeferredInfo final {
    const semantic::FunctionSymbol* function = nullptr;
    std::vector<const core::Expression*> operands;
    std::vector<SemanticType> types;
    std::vector<bool> transfers;
    llvm::StructType* payloadType = nullptr;
    std::size_t slot = 0;
    std::size_t scope = 0;
};

class Emitter final {
   public:
    Emitter(llvm::LLVMContext& context, const llvm::DataLayout& dataLayout,
            const semantic::SemanticModel& model, llvm::Module& module,
            std::vector<FunctionEntry>& entries, std::string& error)
        : _context(context),
          _dataLayout(dataLayout),
          _model(model),
          _module(module),
          _entries(entries),
          _error(error),
          _builder(context),
          _types(context, dataLayout) {
        for (FunctionEntry& entry : entries)
            _functions.emplace(entry.symbol, &entry);
    }

    bool Emit() {
        for (FunctionEntry& entry : _entries)
            for (const semantic::AsyncFrameSlot& slot : entry.plan->slots)
                if (!EmitTears(slot.type)) return false;
        for (FunctionEntry& entry : _entries) DeclareWrappers(entry);
        for (FunctionEntry& entry : _entries)
            if (!EmitFunction(entry)) return false;
        return true;
    }

   private:
    bool Fail(std::string message) {
        if (_error.empty()) _error = std::move(message);
        return false;
    }

    void DeclareWrappers(const FunctionEntry& entry) {
        llvm::Type* pointer = _builder.getPtrTy();
        llvm::Function::Create(
            llvm::FunctionType::get(_builder.getVoidTy(),
                                    {pointer, pointer, pointer}, false),
            llvm::Function::ExternalLinkage,
            entry.generated.name + "_initialize", _module);
        llvm::Function::Create(
            llvm::FunctionType::get(_builder.getInt32Ty(), {pointer, pointer},
                                    false),
            llvm::Function::ExternalLinkage,
            entry.generated.name + "_frame_poll", _module);
        llvm::Function::Create(
            llvm::FunctionType::get(_builder.getVoidTy(), {pointer}, false),
            llvm::Function::ExternalLinkage, entry.generated.name + "_destroy",
            _module);
        if (entry.plan->returnValue)
            llvm::Function::Create(
                llvm::FunctionType::get(_builder.getVoidTy(),
                                        {pointer, pointer}, false),
                llvm::Function::ExternalLinkage, entry.generated.name + "_take",
                _module);
    }

    llvm::Value* BytePointer(std::size_t offset) {
        return _builder.CreateInBoundsGEP(_builder.getInt8Ty(), _frame,
                                          _builder.getInt64(offset));
    }

    llvm::Value* Slot(std::size_t id) {
        if (id >= _entry->generated.frame.slotOffsets.size() ||
            !_entry->generated.frame.slotOffsets[id]) {
            Fail("async value action references a non-value slot");
            return nullptr;
        }
        return BytePointer(*_entry->generated.frame.slotOffsets[id]);
    }

    llvm::Value* Guard(std::size_t id) {
        return BytePointer(_entry->generated.frame.initializedOffset + id);
    }

    std::optional<std::size_t> SlotFor(const semantic::Symbol* symbol) const {
        for (std::size_t id = 0; id < _entry->plan->slots.size(); ++id)
            if (_entry->plan->slots[id].symbol == symbol) return id;
        return std::nullopt;
    }

    DeferredInfo* DeferInfo(const core::DeferStatement& statement) {
        if (const auto found = _defers.find(&statement); found != _defers.end())
            return &found->second;
        DeferredInfo info;
        bool foundSlot = false;
        for (std::size_t slot = 0; slot < _entry->plan->slots.size(); ++slot)
            if (_entry->plan->slots[slot].kind == AsyncSlotKind::DeferredCall &&
                _entry->plan->slots[slot].syntax == &statement) {
                info.slot = slot;
                foundSlot = true;
                if (!_entry->plan->slots[slot].scope)
                    return Fail("async defer has no lexical scope"), nullptr;
                info.scope = *_entry->plan->slots[slot].scope;
                break;
            }
        if (!foundSlot || info.scope >= _entry->plan->scopes.size())
            return Fail("async defer has no frame record"), nullptr;

        const core::Expression& expression = Unwrap(statement.Call());
        if (expression.Kind() == core::AstNodeKind::CallExpression) {
            const auto& call =
                static_cast<const core::CallExpression&>(expression);
            const core::Expression& callee = Unwrap(call.Callee());
            if (callee.Kind() != core::AstNodeKind::NameExpression)
                return Fail(
                           "indirect calls are unsupported in generated "
                           "async defers"),
                       nullptr;
            const semantic::Symbol* symbol = _model.SymbolFor(
                static_cast<const core::NameExpression&>(callee));
            if (!symbol || symbol->Kind() != semantic::SymbolKind::Function)
                return Fail("unresolved generated async defer call"), nullptr;
            info.function =
                static_cast<const semantic::FunctionSymbol*>(symbol);
            info.operands = _model.ArgumentsFor(call);
        } else if (expression.Kind() ==
                   core::AstNodeKind::MemberCallExpression) {
            const auto& call =
                static_cast<const core::MemberCallExpression&>(expression);
            if (_model.InterfaceMethodFor(call))
                return Fail(
                           "interface calls are unsupported in generated "
                           "async defers"),
                       nullptr;
            info.function = _model.MethodFor(call);
            if (info.function && info.function->HasReceiver() &&
                !_model.IsQualifiedInterfaceProviderCall(call))
                info.operands.push_back(&call.Receiver());
            const auto arguments = _model.ArgumentsFor(call);
            info.operands.insert(info.operands.end(), arguments.begin(),
                                 arguments.end());
        } else
            return Fail("generated async defer requires a call"), nullptr;

        if (!info.function || info.function->IsAsync() ||
            info.function->BuiltIn() != semantic::BuiltInFunction::None)
            return Fail(
                       "generated async defer requires a direct synchronous "
                       "function or structure method"),
                   nullptr;
        if (info.operands.size() != info.function->ParameterTypes().size())
            return Fail("generated async defer has invalid operands"), nullptr;
        std::vector<llvm::Type*> fields;
        for (std::size_t index = 0; index < info.operands.size(); ++index) {
            const SemanticType type = info.function->ParameterTypes()[index];
            llvm::Type* mapped = _types.Map(type);
            if (!mapped || !mapped->isSized())
                return Fail("unsupported generated async defer capture type '" +
                            std::string(type.Name()) + "'"),
                       nullptr;
            info.types.push_back(type);
            info.transfers.push_back(_model.TransferOf(*info.operands[index]) ==
                                     semantic::TransferKind::Move);
            fields.push_back(mapped);
        }
        info.payloadType = llvm::StructType::get(_context, fields);
        const auto [inserted, _] = _defers.emplace(&statement, std::move(info));
        return &inserted->second;
    }

    llvm::Value* Address(const core::Expression& raw) {
        const core::Expression& expression = Unwrap(raw);
        if (expression.Kind() == core::AstNodeKind::NameExpression) {
            const auto& name =
                static_cast<const core::NameExpression&>(expression);
            const semantic::Symbol* symbol = _model.SymbolFor(name);
            if (const auto local = _localAddresses.find(symbol);
                local != _localAddresses.end())
                return local->second;
            const auto slot = SlotFor(symbol);
            if (!slot) {
                Fail("async name has no frame slot: " + name.Name());
                return nullptr;
            }
            return Slot(*slot);
        }
        if (expression.Kind() == core::AstNodeKind::MemberAccessExpression) {
            const auto& member =
                static_cast<const core::MemberAccessExpression&>(expression);
            const semantic::StructureField* field = _model.FieldFor(member);
            if (!field) {
                Fail("unsupported async member value");
                return nullptr;
            }
            SemanticType receiverType = _model.TypeOf(member.Receiver());
            llvm::Value* base = nullptr;
            if (receiverType.Kind() == SemanticTypeKind::Reference ||
                member.Access() == TokenType::Arrow)
                base = Eval(member.Receiver());
            else
                base = Address(member.Receiver());
            if (!base) return nullptr;
            const semantic::StructureType* structure =
                receiverType.Kind() == SemanticTypeKind::Reference
                    ? receiverType.Target()->Structure()
                    : receiverType.Structure();
            const std::size_t index = field - structure->fields.data();
            return _builder.CreateStructGEP(_types.Map(SemanticType(structure)),
                                            base, index);
        }
        if (expression.Kind() == core::AstNodeKind::UnaryExpression) {
            const auto& unary =
                static_cast<const core::UnaryExpression&>(expression);
            if (unary.OperatorType() == TokenType::Mul)
                return Eval(unary.Operand());
        }
        Fail("unsupported async place expression");
        return nullptr;
    }

    llvm::Value* AwaitValue(const core::AwaitExpression& await) {
        for (const semantic::AsyncState& state : _entry->plan->states)
            if (state.checked && state.awaitOperation == &await) {
                const std::size_t slot = state.checked->value
                                             ? *state.checked->value
                                             : state.checked->error;
                return _builder.CreateLoad(
                    _types.Map(_entry->plan->slots[slot].type), Slot(slot),
                    state.checked->value ? "await.value" : "await.error");
            }
        Fail("await result has no concrete value slot");
        return nullptr;
    }

    llvm::Value* Literal(const core::LiteralExpression& literal,
                         SemanticType type) {
        llvm::Type* mapped = _types.Map(type);
        if (!mapped) return nullptr;
        if (literal.LiteralType() == TokenType::None ||
            literal.LiteralType() == TokenType::Null)
            return llvm::Constant::getNullValue(mapped);
        if (literal.LiteralType() == TokenType::True ||
            literal.LiteralType() == TokenType::False)
            return llvm::ConstantInt::get(
                mapped, literal.LiteralType() == TokenType::True);
        if (type.IsFloating()) {
            double value = std::stod(literal.Value());
            return llvm::ConstantFP::get(mapped, value);
        }
        std::int64_t value = 0;
        const std::string& text = literal.Value();
        std::from_chars(text.data(), text.data() + text.size(), value);
        return llvm::ConstantInt::get(mapped, value, SignedInteger(type));
    }

    llvm::FunctionType* FunctionType(const semantic::FunctionSymbol& symbol) {
        llvm::Type* result = symbol.ReturnType().ContainsStructure()
                                 ? _builder.getVoidTy()
                                 : _types.Map(symbol.ReturnType());
        if (!result) return nullptr;
        std::vector<llvm::Type*> parameters;
        if (symbol.ReturnType().ContainsStructure())
            parameters.push_back(_builder.getPtrTy());
        for (SemanticType parameter : symbol.ParameterTypes()) {
            llvm::Type* mapped = _types.Map(parameter);
            if (!mapped) return nullptr;
            parameters.push_back(mapped);
        }
        return llvm::FunctionType::get(result, parameters, false);
    }

    bool EmitTears(SemanticType type) {
        if (type.Kind() == SemanticTypeKind::Structure) {
            if (type.Structure()->tear &&
                type.Structure()->tear->Declaration() &&
                !_emittedSync.contains(type.Structure()->tear))
                if (!EmitSyncFunction(*type.Structure()->tear)) return false;
            for (const auto& field : type.Structure()->fields)
                if (!EmitTears(field.type)) return false;
        } else if (type.Kind() == SemanticTypeKind::Tuple) {
            for (SemanticType element : type.Elements())
                if (!EmitTears(element)) return false;
        } else if (type.Kind() == SemanticTypeKind::Array) {
            return EmitTears(*type.Target());
        }
        return true;
    }

    bool EmitSyncFunction(const semantic::FunctionSymbol& symbol) {
        llvm::FunctionType* type = FunctionType(symbol);
        if (!type)
            return Fail(
                "unsupported synchronous helper type in async value "
                "generation");
        llvm::Function* function = _module.getFunction(symbol.LinkName());
        if (!function)
            function =
                llvm::Function::Create(type, llvm::Function::ExternalLinkage,
                                       symbol.LinkName(), _module);
        _emittedSync.insert(&symbol);
        llvm::Function* savedFunction = _function;
        llvm::Value* savedFrame = _frame;
        FunctionEntry* savedEntry = _entry;
        auto savedLocals = std::move(_localAddresses);
        const llvm::IRBuilderBase::InsertPoint savedInsert = _builder.saveIP();
        _function = function;
        _frame = nullptr;
        _localAddresses.clear();
        _builder.SetInsertPoint(
            llvm::BasicBlock::Create(_context, "entry", function));
        const core::FunctionDeclaration& declaration = *symbol.Declaration();
        std::size_t argument = symbol.ReturnType().ContainsStructure() ? 1 : 0;
        std::vector<std::pair<llvm::Value*, SemanticType>> ownedParameters;
        for (const auto& parameter : declaration.Parameters()) {
            const semantic::Symbol* parameterSymbol =
                _model.DeclaredSymbol(*parameter);
            llvm::Value* storage =
                _builder.CreateAlloca(_types.Map(parameterSymbol->Type()),
                                      nullptr, parameterSymbol->Name());
            _builder.CreateStore(function->getArg(argument++), storage);
            _localAddresses.emplace(parameterSymbol, storage);
            if (parameterSymbol->Type().OwnsStorage())
                ownedParameters.emplace_back(storage, parameterSymbol->Type());
        }
        const auto cleanupParameters = [&] {
            for (auto parameter = ownedParameters.rbegin();
                 parameter != ownedParameters.rend(); ++parameter)
                DestroyAt(parameter->first, parameter->second);
        };
        for (const auto& statement : declaration.Body().Statements()) {
            if (statement->Kind() == core::AstNodeKind::ExpressionStatement) {
                const auto& expression =
                    static_cast<const core::ExpressionStatement&>(*statement)
                        .Value();
                if (!Eval(expression)) return false;
            } else if (statement->Kind() ==
                       core::AstNodeKind::ReturnStatement) {
                const auto& result =
                    static_cast<const core::ReturnStatement&>(*statement);
                if (result.Value()) {
                    llvm::Value* value =
                        Eval(*result.Value(), symbol.ReturnType());
                    if (!value) return false;
                    if (symbol.ReturnType().ContainsStructure()) {
                        _builder.CreateStore(value, function->getArg(0));
                        _builder.CreateRetVoid();
                    } else {
                        _builder.CreateRet(value);
                    }
                } else {
                    cleanupParameters();
                    _builder.CreateRetVoid();
                }
                break;
            } else if (statement->Kind() ==
                       core::AstNodeKind::VariableDeclarationStatement) {
                const auto& declaration =
                    static_cast<const core::VariableDeclarationStatement&>(
                        *statement);
                const semantic::Symbol* variable =
                    _model.DeclaredSymbol(declaration);
                llvm::Value* storage = _builder.CreateAlloca(
                    _types.Map(variable->Type()), nullptr, variable->Name());
                llvm::Value* value =
                    declaration.Initializer()
                        ? Eval(*declaration.Initializer(), variable->Type())
                        : llvm::Constant::getNullValue(
                              _types.Map(variable->Type()));
                if (!value) return false;
                _builder.CreateStore(value, storage);
                _localAddresses.emplace(variable, storage);
            } else {
                return Fail(
                    "unsupported statement in synchronous helper used by "
                    "generated async values");
            }
        }
        if (!_builder.GetInsertBlock()->getTerminator()) {
            if (symbol.ReturnType().Kind() == SemanticTypeKind::Void) {
                cleanupParameters();
                _builder.CreateRetVoid();
            } else
                return Fail("synchronous helper has no generated return");
        }
        _localAddresses = std::move(savedLocals);
        _function = savedFunction;
        _frame = savedFrame;
        _entry = savedEntry;
        _builder.restoreIP(savedInsert);
        return true;
    }

    llvm::Value* Call(const core::CallExpression& call) {
        const core::Expression& calleeSyntax = Unwrap(call.Callee());
        if (calleeSyntax.Kind() != core::AstNodeKind::NameExpression) {
            Fail("indirect calls are unsupported in generated async values");
            return nullptr;
        }
        const auto* symbol = _model.SymbolFor(
            static_cast<const core::NameExpression&>(calleeSyntax));
        if (!symbol || symbol->Kind() != semantic::SymbolKind::Function) {
            Fail("unresolved call in generated async value");
            return nullptr;
        }
        const auto& function =
            static_cast<const semantic::FunctionSymbol&>(*symbol);
        if (function.IsAsync()) {
            Fail("async calls must be lowered as direct-await operations");
            return nullptr;
        }
        if (!function.IsExtern() && function.Declaration() &&
            !_emittedSync.contains(&function) && !EmitSyncFunction(function))
            return nullptr;
        llvm::FunctionType* type = FunctionType(function);
        if (!type) {
            Fail("unsupported synchronous call type in async function");
            return nullptr;
        }
        llvm::FunctionCallee callee =
            _module.getOrInsertFunction(function.LinkName(), type);
        std::vector<llvm::Value*> arguments;
        llvm::AllocaInst* aggregate = nullptr;
        if (function.ReturnType().ContainsStructure()) {
            aggregate =
                _builder.CreateAlloca(_types.Map(function.ReturnType()));
            arguments.push_back(aggregate);
        }
        const auto source = _model.ArgumentsFor(call);
        for (std::size_t index = 0; index < source.size(); ++index) {
            const core::Expression* argument = source[index];
            llvm::Value* value =
                Eval(*argument, function.ParameterTypes()[index]);
            if (!value) return nullptr;
            arguments.push_back(value);
        }
        llvm::CallInst* result = _builder.CreateCall(callee, arguments);
        return aggregate
                   ? _builder.CreateLoad(_types.Map(function.ReturnType()),
                                         aggregate, "call.aggregate")
                   : static_cast<llvm::Value*>(result);
    }

    bool InvokeDeferred(DeferredInfo& deferred, llvm::Value* payload) {
        const semantic::FunctionSymbol& function = *deferred.function;
        if (!function.IsExtern() && function.Declaration() &&
            !_emittedSync.contains(&function) && !EmitSyncFunction(function))
            return false;
        llvm::FunctionType* type = FunctionType(function);
        if (!type)
            return Fail(
                "unsupported synchronous deferred call type in async "
                "function");
        llvm::FunctionCallee callee =
            _module.getOrInsertFunction(function.LinkName(), type);
        std::vector<llvm::Value*> arguments;
        llvm::AllocaInst* resultStorage = nullptr;
        if (function.ReturnType().ContainsStructure()) {
            resultStorage =
                _builder.CreateAlloca(_types.Map(function.ReturnType()));
            arguments.push_back(resultStorage);
        }
        for (std::size_t index = 0; index < deferred.types.size(); ++index) {
            llvm::Value* address =
                _builder.CreateStructGEP(deferred.payloadType, payload, index);
            arguments.push_back(_builder.CreateLoad(
                _types.Map(deferred.types[index]), address, "defer.capture"));
        }
        llvm::CallInst* result = _builder.CreateCall(callee, arguments);
        if (resultStorage)
            DestroyAt(resultStorage, function.ReturnType());
        else if (function.ReturnType().OwnsStorage()) {
            llvm::AllocaInst* storage =
                _builder.CreateAlloca(_types.Map(function.ReturnType()));
            _builder.CreateStore(result, storage);
            DestroyAt(storage, function.ReturnType());
        }
        for (std::size_t index = deferred.types.size(); index-- > 0;)
            if (deferred.types[index].OwnsStorage() &&
                !deferred.transfers[index])
                DestroyAt(_builder.CreateStructGEP(deferred.payloadType,
                                                   payload, index),
                          deferred.types[index]);
        return true;
    }

    llvm::Value* Eval(const core::Expression& raw,
                      SemanticType expected = SemanticType::Error()) {
        const core::Expression& expression = Unwrap(raw);
        SemanticType type = _model.TypeOf(expression);
        if (!expected.IsError() &&
            expected.Kind() == SemanticTypeKind::Reference &&
            type.Kind() != SemanticTypeKind::Reference)
            return Address(expression);
        if (!expected.IsError() && (type.Kind() == SemanticTypeKind::None ||
                                    type.Kind() == SemanticTypeKind::Null))
            type = expected;
        switch (expression.Kind()) {
            case core::AstNodeKind::LiteralExpression:
                return Literal(
                    static_cast<const core::LiteralExpression&>(expression),
                    type);
            case core::AstNodeKind::NameExpression: {
                llvm::Value* address = Address(expression);
                return address ? _builder.CreateLoad(_types.Map(type), address)
                               : nullptr;
            }
            case core::AstNodeKind::MemberAccessExpression: {
                llvm::Value* address = Address(expression);
                return address ? _builder.CreateLoad(_types.Map(type), address)
                               : nullptr;
            }
            case core::AstNodeKind::AwaitExpression:
                return AwaitValue(
                    static_cast<const core::AwaitExpression&>(expression));
            case core::AstNodeKind::CallExpression:
                return Call(
                    static_cast<const core::CallExpression&>(expression));
            case core::AstNodeKind::TupleExpression: {
                const auto& tuple =
                    static_cast<const core::TupleExpression&>(expression);
                llvm::Value* value = llvm::PoisonValue::get(_types.Map(type));
                for (std::size_t index = 0; index < tuple.Elements().size();
                     ++index) {
                    llvm::Value* element =
                        Eval(*tuple.Elements()[index], type.Elements()[index]);
                    if (!element) return nullptr;
                    value = _builder.CreateInsertValue(value, element, index);
                }
                return value;
            }
            case core::AstNodeKind::CastExpression: {
                const auto& cast =
                    static_cast<const core::CastExpression&>(expression);
                llvm::Value* value = Eval(cast.Value());
                if (!value) return nullptr;
                llvm::Type* target = _types.Map(type);
                if (value->getType() == target) return value;
                if (value->getType()->isIntegerTy() && target->isIntegerTy())
                    return _builder.CreateIntCast(
                        value, target,
                        SignedInteger(_model.TypeOf(cast.Value())));
                if (value->getType()->isIntegerTy() &&
                    target->isFloatingPointTy())
                    return SignedInteger(_model.TypeOf(cast.Value()))
                               ? _builder.CreateSIToFP(value, target)
                               : _builder.CreateUIToFP(value, target);
                return _builder.CreateBitCast(value, target);
            }
            case core::AstNodeKind::UnaryExpression:
                return Unary(
                    static_cast<const core::UnaryExpression&>(expression));
            case core::AstNodeKind::BinaryExpression:
                return Binary(
                    static_cast<const core::BinaryExpression&>(expression));
            case core::AstNodeKind::AssignmentExpression:
                return Assignment(
                    static_cast<const core::AssignmentExpression&>(expression));
            case core::AstNodeKind::TryExpression:
                return Eval(static_cast<const core::TryExpression&>(expression)
                                .Operand());
            default:
                Fail("unsupported async value expression kind " +
                     std::to_string(static_cast<unsigned>(expression.Kind())));
                return nullptr;
        }
    }

    llvm::Value* Unary(const core::UnaryExpression& unary) {
        if (unary.OperatorType() == TokenType::Ampersand ||
            unary.OperatorType() == TokenType::RawAddress)
            return Address(unary.Operand());
        if (unary.OperatorType() == TokenType::Mul) {
            llvm::Value* address = Eval(unary.Operand());
            return address ? _builder.CreateLoad(
                                 _types.Map(_model.TypeOf(unary)), address)
                           : nullptr;
        }
        if (unary.OperatorType() == TokenType::Increment ||
            unary.OperatorType() == TokenType::Decrement) {
            llvm::Value* address = Address(unary.Operand());
            if (!address) return nullptr;
            llvm::Type* type = _types.Map(_model.TypeOf(unary));
            llvm::Value* previous = _builder.CreateLoad(type, address);
            llvm::Value* next =
                unary.OperatorType() == TokenType::Increment
                    ? _builder.CreateAdd(previous,
                                         llvm::ConstantInt::get(type, 1))
                    : _builder.CreateSub(previous,
                                         llvm::ConstantInt::get(type, 1));
            _builder.CreateStore(next, address);
            return previous;
        }
        llvm::Value* value = Eval(unary.Operand());
        if (!value) return nullptr;
        if (unary.OperatorType() == TokenType::Bang)
            return _builder.CreateNot(value);
        if (unary.OperatorType() == TokenType::Sub)
            return value->getType()->isFloatingPointTy()
                       ? _builder.CreateFNeg(value)
                       : _builder.CreateNeg(value);
        if (unary.OperatorType() == TokenType::BitNot)
            return _builder.CreateNot(value);
        return value;
    }

    llvm::Value* Binary(const core::BinaryExpression& binary) {
        const SemanticType leftType = _model.TypeOf(binary.Left());
        const SemanticType rightType = _model.TypeOf(binary.Right());
        llvm::Value* left =
            Eval(binary.Left(), rightType.Kind() == SemanticTypeKind::None
                                    ? SemanticType::Error()
                                    : rightType);
        llvm::Value* right =
            Eval(binary.Right(), leftType.Kind() == SemanticTypeKind::None
                                     ? SemanticType::Error()
                                     : leftType);
        if (!left || !right) return nullptr;
        if (_model.TypeOf(binary.Left()).Kind() ==
                SemanticTypeKind::ErrorValue ||
            _model.TypeOf(binary.Right()).Kind() ==
                SemanticTypeKind::ErrorValue) {
            if (left->getType()->isStructTy())
                left = _builder.CreateExtractValue(left, 1);
            if (right->getType()->isStructTy())
                right = _builder.CreateExtractValue(right, 1);
        }
        const bool floating = left->getType()->isFloatingPointTy();
        const bool sign = SignedInteger(_model.TypeOf(binary.Left()));
        switch (binary.OperatorType()) {
            case TokenType::Add:
                return floating ? _builder.CreateFAdd(left, right)
                                : _builder.CreateAdd(left, right);
            case TokenType::Sub:
                return floating ? _builder.CreateFSub(left, right)
                                : _builder.CreateSub(left, right);
            case TokenType::Mul:
                return floating ? _builder.CreateFMul(left, right)
                                : _builder.CreateMul(left, right);
            case TokenType::Div:
                return floating ? _builder.CreateFDiv(left, right)
                       : sign   ? _builder.CreateSDiv(left, right)
                                : _builder.CreateUDiv(left, right);
            case TokenType::Mod:
                return sign ? _builder.CreateSRem(left, right)
                            : _builder.CreateURem(left, right);
            case TokenType::Equal:
            case TokenType::StrictEqual:
                return floating ? _builder.CreateFCmpOEQ(left, right)
                                : _builder.CreateICmpEQ(left, right);
            case TokenType::NotEqual:
            case TokenType::StrictNotEqual:
                return floating ? _builder.CreateFCmpUNE(left, right)
                                : _builder.CreateICmpNE(left, right);
            case TokenType::Less:
                return floating ? _builder.CreateFCmpOLT(left, right)
                       : sign   ? _builder.CreateICmpSLT(left, right)
                                : _builder.CreateICmpULT(left, right);
            case TokenType::LessEqual:
                return floating ? _builder.CreateFCmpOLE(left, right)
                       : sign   ? _builder.CreateICmpSLE(left, right)
                                : _builder.CreateICmpULE(left, right);
            case TokenType::Greater:
                return floating ? _builder.CreateFCmpOGT(left, right)
                       : sign   ? _builder.CreateICmpSGT(left, right)
                                : _builder.CreateICmpUGT(left, right);
            case TokenType::GreaterEqual:
                return floating ? _builder.CreateFCmpOGE(left, right)
                       : sign   ? _builder.CreateICmpSGE(left, right)
                                : _builder.CreateICmpUGE(left, right);
            case TokenType::And:
                return _builder.CreateAnd(left, right);
            case TokenType::Or:
            case TokenType::BitOr:
                return _builder.CreateOr(left, right);
            case TokenType::Xor:
                return _builder.CreateXor(left, right);
            case TokenType::ShiftLeft:
                return _builder.CreateShl(left, right);
            case TokenType::ShiftRight:
                return sign ? _builder.CreateAShr(left, right)
                            : _builder.CreateLShr(left, right);
            default:
                Fail("unsupported async binary operator");
                return nullptr;
        }
    }

    llvm::Value* Assignment(const core::AssignmentExpression& assignment) {
        llvm::Value* address = Address(assignment.Target());
        llvm::Value* value =
            Eval(assignment.Value(), _model.TypeOf(assignment.Target()));
        if (!address || !value) return nullptr;
        if (assignment.OperatorType() != TokenType::Assign) {
            llvm::Value* previous =
                _builder.CreateLoad(value->getType(), address);
            switch (assignment.OperatorType()) {
                case TokenType::AddAssign:
                    value = _builder.CreateAdd(previous, value);
                    break;
                case TokenType::SubAssign:
                    value = _builder.CreateSub(previous, value);
                    break;
                case TokenType::MulAssign:
                    value = _builder.CreateMul(previous, value);
                    break;
                case TokenType::DivAssign:
                    value = _builder.CreateSDiv(previous, value);
                    break;
                default:
                    Fail("unsupported async compound assignment");
                    return nullptr;
            }
        }
        _builder.CreateStore(value, address);
        return value;
    }

    void DestroyAt(llvm::Value* storage, SemanticType type) {
        if (type.Kind() == SemanticTypeKind::Structure) {
            if (type.Structure()->tear) {
                llvm::FunctionType* signature =
                    FunctionType(*type.Structure()->tear);
                llvm::FunctionCallee tear = _module.getOrInsertFunction(
                    type.Structure()->tear->LinkName(), signature);
                _builder.CreateCall(tear, {storage});
            }
            for (std::size_t index = type.Structure()->fields.size(); index > 0;
                 --index) {
                SemanticType field = type.Structure()->fields[index - 1].type;
                if (field.OwnsStorage())
                    DestroyAt(_builder.CreateStructGEP(_types.Map(type),
                                                       storage, index - 1),
                              field);
            }
        } else if (type.Kind() == SemanticTypeKind::Tuple) {
            for (std::size_t index = type.Elements().size(); index > 0; --index)
                if (type.Elements()[index - 1].OwnsStorage())
                    DestroyAt(_builder.CreateStructGEP(_types.Map(type),
                                                       storage, index - 1),
                              type.Elements()[index - 1]);
        } else if (type.Kind() == SemanticTypeKind::Array &&
                   type.Target()->OwnsStorage()) {
            for (std::size_t index = type.Length(); index > 0; --index)
                DestroyAt(
                    _builder.CreateInBoundsGEP(
                        _types.Map(type), storage,
                        {_builder.getInt64(0), _builder.getInt64(index - 1)}),
                    *type.Target());
        }
    }

    llvm::Value* ErrorCode(llvm::Value* error, SemanticType type) {
        if (type.Kind() == SemanticTypeKind::ErrorValue)
            return _builder.CreateExtractValue(error, 1);
        return error;
    }

    llvm::Value* CoerceError(llvm::Value* value, SemanticType source,
                             SemanticType destination) {
        if (source == destination) return value;
        if (source.Kind() == SemanticTypeKind::NamedError &&
            destination.Kind() == SemanticTypeKind::ErrorValue) {
            const std::string name = ".flecha.error." + source.Family().name;
            llvm::GlobalVariable* identity = _module.getNamedGlobal(name);
            if (!identity)
                identity = new llvm::GlobalVariable(
                    _module, _builder.getInt8Ty(), true,
                    llvm::GlobalValue::PrivateLinkage,
                    llvm::ConstantInt::get(_builder.getInt8Ty(), 0), name);
            llvm::Value* family = _builder.CreateSelect(
                _builder.CreateICmpEQ(
                    value, llvm::Constant::getNullValue(value->getType())),
                llvm::ConstantPointerNull::get(_builder.getPtrTy()), identity,
                "error.family");
            llvm::Value* result =
                llvm::PoisonValue::get(_types.Map(destination));
            result = _builder.CreateInsertValue(result, family, 0);
            return _builder.CreateInsertValue(result, value, 1, "error.value");
        }
        return value;
    }

    bool IsAwait(const core::Expression* expression) const {
        return expression &&
               Unwrap(*expression).Kind() == core::AstNodeKind::AwaitExpression;
    }

    bool Store(std::size_t slot, llvm::Value* value) {
        if (!value) return false;
        llvm::Value* storage = Slot(slot);
        if (!storage) return false;
        _builder.CreateStore(value, storage);
        return true;
    }

    std::uint8_t Flow(std::size_t stateId) {
        if (stateId >= _entry->plan->states.size()) return 1;
        const semantic::AsyncState& state = _entry->plan->states[stateId];
        if (!state.flowNode) return 1;
        const semantic::FlowNode& node =
            _entry->plan->flow.nodes[*state.flowNode];
        if (node.condition) {
            if (_entry->machine.Instructions()[stateId].kind !=
                lowering::AsyncInstructionKind::Branch)
                return 1;
            llvm::Value* condition = Eval(*node.condition);
            if (!condition) return 0;
            _flowResult = _builder.CreateZExt(condition, _builder.getInt8Ty());
            return 2;
        }
        if (node.propagationDispatch) {
            llvm::Value* value = Eval(node.propagationDispatch->Operand());
            SemanticType operand =
                _model.TypeOf(node.propagationDispatch->Operand());
            if (!value || !_entry->plan->returnValue) return 0;
            llvm::Value* error = value;
            if (operand.Kind() == SemanticTypeKind::Tuple)
                error = _builder.CreateExtractValue(
                    value, operand.Elements().size() - 1);
            const SemanticType sourceError =
                operand.Kind() == SemanticTypeKind::Tuple
                    ? operand.Elements().back()
                    : operand;
            const SemanticType destination =
                _entry->plan->slots[*_entry->plan->returnValue].type;
            const SemanticType destinationError =
                destination.Kind() == SemanticTypeKind::Tuple
                    ? destination.Elements().back()
                    : destination;
            llvm::Value* storedError =
                CoerceError(error, sourceError, destinationError);
            llvm::Value* returned = storedError;
            if (destination.Kind() == SemanticTypeKind::Tuple) {
                returned =
                    llvm::Constant::getNullValue(_types.Map(destination));
                returned = _builder.CreateInsertValue(
                    returned, storedError, destination.Elements().size() - 1);
            }
            if (!Store(*_entry->plan->returnValue, returned)) return 0;
            if (const auto* binding = node.propagationDispatch->PeekBinding()) {
                const auto* symbol = _model.DeclaredSymbol(*binding);
                const auto slot = SlotFor(symbol);
                if (!slot || !Store(*slot, CoerceError(error, sourceError,
                                                       symbol->Type())))
                    return 0;
            }
            llvm::Value* code = ErrorCode(error, sourceError);
            _flowResult = _builder.CreateZExt(
                _builder.CreateICmpEQ(
                    code, llvm::Constant::getNullValue(code->getType())),
                _builder.getInt8Ty());
            return 2;
        }
        if (node.propagationReturn && node.propagationReturn->PeekBinding()) {
            if (!_entry->plan->returnValue) return 0;
            const auto* symbol =
                _model.DeclaredSymbol(*node.propagationReturn->PeekBinding());
            const auto slot = SlotFor(symbol);
            if (!slot) return 0;
            llvm::Value* error = _builder.CreateLoad(
                _types.Map(symbol->Type()), Slot(*slot), "try.peek.error");
            const SemanticType destination =
                _entry->plan->slots[*_entry->plan->returnValue].type;
            const SemanticType destinationError =
                destination.Kind() == SemanticTypeKind::Tuple
                    ? destination.Elements().back()
                    : destination;
            error = CoerceError(error, symbol->Type(), destinationError);
            if (destination.Kind() != SemanticTypeKind::Tuple)
                return Store(*_entry->plan->returnValue, error) ? 1 : 0;
            llvm::Value* returned =
                llvm::Constant::getNullValue(_types.Map(destination));
            returned = _builder.CreateInsertValue(
                returned, error, destination.Elements().size() - 1);
            return Store(*_entry->plan->returnValue, returned) ? 1 : 0;
        }
        if (node.memoryDeclaration && node.write) {
            const auto slot = SlotFor(node.write);
            const core::Expression* initializer =
                node.memoryDeclaration->Initializer();
            if (slot && initializer &&
                !Store(*slot,
                       Eval(*initializer, _entry->plan->slots[*slot].type)))
                return 0;
            return 1;
        }
        if (node.memoryReturn && node.memoryReturn->Value() &&
            _entry->plan->returnValue) {
            if (!Store(
                    *_entry->plan->returnValue,
                    Eval(*node.memoryReturn->Value(),
                         _entry->plan->slots[*_entry->plan->returnValue].type)))
                return 0;
            return 1;
        }
        if (node.memoryDestructure) {
            if (_model.CheckedResultFor(*node.memoryDestructure)) return 1;
            llvm::Value* value = Eval(node.memoryDestructure->Value());
            SemanticType type = _model.TypeOf(node.memoryDestructure->Value());
            if (!value) return 0;
            for (std::size_t index = 0;
                 index < node.memoryDestructure->Bindings().size(); ++index) {
                const semantic::Symbol* symbol = _model.DestructureTarget(
                    *node.memoryDestructure->Bindings()[index]);
                const auto slot = SlotFor(symbol);
                if (slot)
                    Store(*slot, _builder.CreateExtractValue(value, index));
            }
            return 1;
        }
        if (node.memoryDiscard && !IsAwait(node.memoryDiscard)) {
            if (!Eval(*node.memoryDiscard)) return 0;
            return 1;
        }
        if (node.memoryExpression &&
            _rootExpressions.contains(node.memoryExpression) &&
            !_terminalExpressions.contains(node.memoryExpression)) {
            if (!Eval(*node.memoryExpression)) return 0;
        }
        return 1;
    }

    llvm::FunctionCallee Runtime(const char* name, llvm::FunctionType* type) {
        return _module.getOrInsertFunction(name, type);
    }

    llvm::Value* OperationStorage(std::size_t state) {
        const auto& instruction = _entry->machine.Instructions()[state];
        if (!instruction.operation) {
            Fail("async await has no operation storage");
            return nullptr;
        }
        return BytePointer(
            _entry->generated.frame.operationOffsets[*instruction.operation]);
    }

    llvm::Value* ChildScopeStorage(std::size_t state) {
        const semantic::AsyncState& source = _entry->plan->states[state];
        if (!source.storage || _entry->plan->slots[*source.storage].kind !=
                                   AsyncSlotKind::TaskScope) {
            Fail("child creation has no lexical scope storage");
            return nullptr;
        }
        return Slot(*source.storage);
    }

    llvm::Value* ChildReservation(std::size_t state) {
        if (state >= _entry->generated.frame.childCreationOffsets.size() ||
            !_entry->generated.frame.childCreationOffsets[state]) {
            Fail("child creation has no reservation storage");
            return nullptr;
        }
        return BytePointer(
            *_entry->generated.frame.childCreationOffsets[state]);
    }

    const core::DeferStatement* DeferredStatement(std::size_t state) const {
        if (state >= _entry->plan->states.size() ||
            !_entry->plan->states[state].flowNode)
            return nullptr;
        return _entry->plan->flow.nodes[*_entry->plan->states[state].flowNode]
            .memoryDefer;
    }

    bool EmitDeferReserve(std::size_t state) {
        const core::DeferStatement* statement = DeferredStatement(state);
        if (!statement) return false;
        DeferredInfo* deferred = DeferInfo(*statement);
        if (!deferred) return false;
        const llvm::StructLayout* layout =
            _dataLayout.getStructLayout(deferred->payloadType);
        const std::size_t size =
            std::max<std::size_t>(1, layout->getSizeInBytes());
        const std::size_t alignment = layout->getAlignment().value();
        llvm::Value* resource = _builder.CreateLoad(
            _builder.getPtrTy(),
            BytePointer(_entry->generated.frame.resourceOffset));
        auto* type = llvm::FunctionType::get(
            _builder.getInt1Ty(),
            {_builder.getPtrTy(), _builder.getPtrTy(), _builder.getInt64Ty(),
             _builder.getInt64Ty(), _builder.getInt64Ty()},
            false);
        llvm::Value* reserved = _builder.CreateCall(
            Runtime("flecha_async_defer_reserve", type),
            {Slot(deferred->slot), resource, _builder.getInt64(deferred->slot),
             _builder.getInt64(size), _builder.getInt64(alignment)});
        _actionResult = _builder.CreateZExt(reserved, _builder.getInt8Ty());
        return true;
    }

    bool EmitDeferInitialize(std::size_t state) {
        const core::DeferStatement* statement = DeferredStatement(state);
        if (!statement) return false;
        DeferredInfo* deferred = DeferInfo(*statement);
        if (!deferred) return false;
        llvm::Value* payload = _builder.CreateLoad(
            _builder.getPtrTy(), Slot(deferred->slot), "defer.payload");
        for (std::size_t index = 0; index < deferred->operands.size();
             ++index) {
            llvm::Value* value =
                Eval(*deferred->operands[index], deferred->types[index]);
            if (!value) return false;
            _builder.CreateStore(
                value, _builder.CreateStructGEP(deferred->payloadType, payload,
                                                index));
        }
        const auto stack = _entry->plan->scopes[deferred->scope].deferredStack;
        if (!stack)
            return Fail("generated async defer scope has no stack storage");
        auto* type = llvm::FunctionType::get(
            _builder.getVoidTy(), {_builder.getPtrTy(), _builder.getPtrTy()},
            false);
        _builder.CreateCall(Runtime("flecha_async_defer_initialize", type),
                            {Slot(*stack), Slot(deferred->slot)});
        return true;
    }

    bool EmitRunDefers(llvm::Value* index) {
        llvm::BasicBlock* invalid = llvm::BasicBlock::Create(
            _context, "defer.scope.invalid", _function);
        auto* select =
            _builder.CreateSwitch(index, invalid, _entry->plan->scopes.size());
        for (std::size_t scope = 0; scope < _entry->plan->scopes.size();
             ++scope) {
            const auto stack = _entry->plan->scopes[scope].deferredStack;
            if (!stack) continue;
            llvm::BasicBlock* entry = llvm::BasicBlock::Create(
                _context, "defer.scope." + std::to_string(scope), _function);
            llvm::BasicBlock* check =
                llvm::BasicBlock::Create(_context, "defer.check", _function);
            llvm::BasicBlock* dispatch =
                llvm::BasicBlock::Create(_context, "defer.dispatch", _function);
            llvm::BasicBlock* done =
                llvm::BasicBlock::Create(_context, "defer.done", _function);
            llvm::BasicBlock* badTag = llvm::BasicBlock::Create(
                _context, "defer.tag.invalid", _function);
            select->addCase(_builder.getInt64(scope), entry);
            _builder.SetInsertPoint(entry);
            _builder.CreateBr(check);
            _builder.SetInsertPoint(check);
            auto* popType = llvm::FunctionType::get(
                _builder.getPtrTy(), {_builder.getPtrTy()}, false);
            llvm::Value* payload =
                _builder.CreateCall(Runtime("flecha_async_defer_pop", popType),
                                    {Slot(*stack)}, "defer.pending");
            _builder.CreateCondBr(
                _builder.CreateICmpNE(
                    payload, llvm::ConstantPointerNull::get(
                                 llvm::PointerType::getUnqual(_context))),
                dispatch, done);
            _builder.SetInsertPoint(dispatch);
            auto* tagType = llvm::FunctionType::get(
                _builder.getInt64Ty(), {_builder.getPtrTy()}, false);
            llvm::Value* tag = _builder.CreateCall(
                Runtime("flecha_async_defer_tag", tagType), {payload});
            std::vector<DeferredInfo*> sites;
            for (const semantic::AsyncFrameSlot& slot : _entry->plan->slots) {
                if (slot.kind != AsyncSlotKind::DeferredCall ||
                    slot.scope != scope || !slot.syntax)
                    continue;
                DeferredInfo* deferred = DeferInfo(
                    *static_cast<const core::DeferStatement*>(slot.syntax));
                if (!deferred) return false;
                sites.push_back(deferred);
            }
            auto* tags = _builder.CreateSwitch(tag, badTag, sites.size());
            for (DeferredInfo* deferred : sites) {
                llvm::BasicBlock* invoke = llvm::BasicBlock::Create(
                    _context, "defer.invoke", _function);
                tags->addCase(_builder.getInt64(deferred->slot), invoke);
                _builder.SetInsertPoint(invoke);
                if (!InvokeDeferred(*deferred, payload)) return false;
                auto* releaseType = llvm::FunctionType::get(
                    _builder.getVoidTy(), {_builder.getPtrTy()}, false);
                _builder.CreateCall(
                    Runtime("flecha_async_defer_release", releaseType),
                    {payload});
                _builder.CreateBr(check);
            }
            _builder.SetInsertPoint(badTag);
            _builder.CreateRet(_builder.getInt8(0));
            _builder.SetInsertPoint(done);
            _builder.CreateRet(_builder.getInt8(1));
        }
        _builder.SetInsertPoint(invalid);
        _builder.CreateRet(_builder.getInt8(0));
        return true;
    }

    bool EmitReserve(std::size_t state) {
        if (DeferredStatement(state)) return EmitDeferReserve(state);
        const semantic::AsyncState& source = _entry->plan->states[state];
        if (!source.checked || !source.checked->callee) return false;
        const auto found = _functions.find(source.checked->callee);
        if (found == _functions.end())
            return Fail("direct await callee has no generated frame");
        const FunctionEntry& child = *found->second;
        llvm::Type* pointer = _builder.getPtrTy();
        llvm::Value* take = nullptr;
        if (child.symbol->ReturnType().Kind() == SemanticTypeKind::Void)
            take = llvm::ConstantPointerNull::get(
                llvm::PointerType::getUnqual(_context));
        else
            take = _module.getFunction(child.generated.name + "_take");
        llvm::Value* reserved = nullptr;
        if (source.kind == AsyncStateKind::CreateChild) {
            llvm::Value* scope = ChildScopeStorage(state);
            llvm::Value* reservation = ChildReservation(state);
            if (!scope || !reservation) return false;
            auto* reserveType = llvm::FunctionType::get(
                _builder.getInt1Ty(),
                {pointer, pointer, pointer, _builder.getInt64Ty(),
                 _builder.getInt64Ty(), pointer, pointer, pointer, pointer},
                false);
            reserved = _builder.CreateCall(
                Runtime("flecha_async_child_reserve", reserveType),
                {scope, reservation, _contextValue,
                 _builder.getInt64(child.generated.frame.size),
                 _builder.getInt64(child.generated.frame.alignment),
                 _module.getFunction(child.generated.name + "_frame_poll"),
                 _module.getFunction(child.generated.name + "_destroy"), take,
                 _module.getFunction(child.generated.name + "_initialize")});
        } else {
            llvm::Value* operation = OperationStorage(state);
            if (!operation) return false;
            auto* reserveType = llvm::FunctionType::get(
                _builder.getInt1Ty(),
                {pointer, _builder.getInt64Ty(), _builder.getInt64Ty(), pointer,
                 pointer, pointer, pointer, pointer},
                false);
            llvm::Value* resource = _builder.CreateLoad(
                pointer, BytePointer(_entry->generated.frame.resourceOffset));
            reserved = _builder.CreateCall(
                Runtime("flecha_async_computation_reserve", reserveType),
                {operation, _builder.getInt64(child.generated.frame.size),
                 _builder.getInt64(child.generated.frame.alignment),
                 _module.getFunction(child.generated.name + "_frame_poll"),
                 _module.getFunction(child.generated.name + "_destroy"), take,
                 _module.getFunction(child.generated.name + "_initialize"),
                 resource});
        }
        _actionResult = _builder.CreateZExt(reserved, _builder.getInt8Ty());
        return true;
    }

    bool EmitInitialize(std::size_t state) {
        if (DeferredStatement(state)) return EmitDeferInitialize(state);
        const semantic::AsyncState& source = _entry->plan->states[state];
        if (!source.checked) return false;
        if (source.kind == AsyncStateKind::ChildAwait) {
            if (!source.checked->consumedChild) return false;
            llvm::Value* operation = OperationStorage(state);
            if (!operation) return false;
            auto* type = llvm::FunctionType::get(
                _builder.getVoidTy(),
                {_builder.getPtrTy(), _builder.getPtrTy()}, false);
            llvm::Value* child = _builder.CreateLoad(
                _builder.getPtrTy(), Slot(*source.checked->consumedChild));
            _builder.CreateCall(
                Runtime("flecha_async_child_join_initialize", type),
                {operation, child});
            return true;
        }
        if (!source.checked->callee) return false;
        llvm::Value* arguments = llvm::ConstantPointerNull::get(
            llvm::PointerType::getUnqual(_context));
        for (std::size_t index = 0; index < source.checked->arguments.size();
             ++index) {
            const auto temporary = std::ranges::find_if(
                _entry->generated.frame.temporaries,
                [state, index](const AsyncTemporaryLayout& value) {
                    return value.state == state && value.argument == index;
                });
            if (temporary == _entry->generated.frame.temporaries.end())
                return Fail("async operation argument has no frame temporary");
            llvm::Value* value =
                Eval(*source.checked->arguments[index],
                     source.checked->callee->ParameterTypes()[index]);
            if (!value) return false;
            llvm::Value* storage = BytePointer(temporary->offset);
            _builder.CreateStore(value, storage);
            if (index == 0) arguments = storage;
        }
        if (source.kind == AsyncStateKind::CreateChild) {
            llvm::Value* scope = ChildScopeStorage(state);
            llvm::Value* reservation = ChildReservation(state);
            if (!scope || !reservation || !source.checked->value) return false;
            auto* type = llvm::FunctionType::get(
                _builder.getVoidTy(),
                {_builder.getPtrTy(), _builder.getPtrTy(), _builder.getPtrTy(),
                 _builder.getPtrTy()},
                false);
            _builder.CreateCall(
                Runtime("flecha_async_child_initialize", type),
                {scope, reservation, arguments, Slot(*source.checked->value)});
        } else {
            llvm::Value* operation = OperationStorage(state);
            if (!operation) return false;
            auto* type = llvm::FunctionType::get(
                _builder.getVoidTy(),
                {_builder.getPtrTy(), _builder.getPtrTy()}, false);
            _builder.CreateCall(
                Runtime("flecha_async_computation_initialize", type),
                {operation, arguments});
        }
        return true;
    }

    bool EmitPoll(std::size_t state, bool cancel) {
        const semantic::AsyncState& source = _entry->plan->states[state];
        llvm::Value* operation = OperationStorage(state);
        if (!operation) return false;
        auto* type = llvm::FunctionType::get(
            _builder.getInt8Ty(), {_builder.getPtrTy(), _builder.getPtrTy()},
            false);
        const char* name = nullptr;
        if (source.kind == AsyncStateKind::ChildAwait)
            name = "flecha_async_child_join_poll";
        else
            name = cancel ? "flecha_async_computation_cancel"
                          : "flecha_async_computation_poll";
        _actionResult = _builder.CreateCall(Runtime(name, type),
                                            {operation, _contextValue});
        return true;
    }

    bool EmitFinish(std::size_t state, llvm::Value* outcome) {
        const semantic::AsyncState& source = _entry->plan->states[state];
        if (!source.checked) return false;
        const bool creation = source.kind == AsyncStateKind::CreateChild;
        const bool join = source.kind == AsyncStateKind::ChildAwait;
        llvm::Value* operation = nullptr;
        if (!creation) {
            operation = OperationStorage(state);
            if (!operation) return false;
        }
        llvm::BasicBlock* ready =
            llvm::BasicBlock::Create(_context, "finish.ready", _function);
        llvm::BasicBlock* failure =
            llvm::BasicBlock::Create(_context, "finish.failure", _function);
        llvm::BasicBlock* done =
            llvm::BasicBlock::Create(_context, "finish.done", _function);
        _builder.CreateCondBr(
            _builder.CreateICmpEQ(outcome, _builder.getInt8(1)), ready,
            failure);
        _builder.SetInsertPoint(ready);
        if (!creation && source.checked->value) {
            auto* takeType = llvm::FunctionType::get(
                _builder.getInt1Ty(),
                {_builder.getPtrTy(), _builder.getPtrTy()}, false);
            _builder.CreateCall(Runtime(join ? "flecha_async_child_join_take"
                                             : "flecha_async_computation_take",
                                        takeType),
                                {operation, Slot(*source.checked->value)});
        }
        _builder.CreateStore(
            llvm::Constant::getNullValue(
                _types.Map(_entry->plan->slots[source.checked->error].type)),
            Slot(source.checked->error));
        _builder.CreateBr(done);
        _builder.SetInsertPoint(failure);
        SemanticType errorType =
            _entry->plan->slots[source.checked->error].type;
        llvm::Value* error =
            llvm::Constant::getNullValue(_types.Map(errorType));
        if (errorType.Kind() == SemanticTypeKind::ErrorValue)
            error = _builder.CreateInsertValue(
                error, _builder.CreateZExt(outcome, _builder.getInt32Ty()), 1);
        else if (errorType.Kind() == SemanticTypeKind::NamedError)
            error = _builder.CreateZExt(outcome, _types.Map(errorType));
        _builder.CreateStore(error, Slot(source.checked->error));
        _builder.CreateBr(done);
        _builder.SetInsertPoint(done);
        if (!creation) {
            auto* releaseType = llvm::FunctionType::get(
                _builder.getInt1Ty(), {_builder.getPtrTy()}, false);
            _builder.CreateCall(
                Runtime(join ? "flecha_async_child_join_release"
                             : "flecha_async_computation_release",
                        releaseType),
                {operation});
        }
        return true;
    }

    bool EmitAction() {
        llvm::Type* pointer = _builder.getPtrTy();
        auto* type = llvm::FunctionType::get(
            _builder.getInt8Ty(),
            {pointer, pointer, _builder.getInt8Ty(), _builder.getInt64Ty(),
             _builder.getInt8Ty()},
            false);
        _function =
            llvm::Function::Create(type, llvm::Function::InternalLinkage,
                                   _entry->generated.name + "_action", _module);
        _function->addFnAttr(llvm::Attribute::NoUnwind);
        _frame = _function->getArg(0);
        _contextValue = _function->getArg(1);
        llvm::Value* action = _function->getArg(2);
        llvm::Value* index = _function->getArg(3);
        llvm::Value* outcome = _function->getArg(4);
        llvm::BasicBlock* entry =
            llvm::BasicBlock::Create(_context, "entry", _function);
        llvm::BasicBlock* invalid =
            llvm::BasicBlock::Create(_context, "invalid", _function);
        _builder.SetInsertPoint(entry);
        auto* select = _builder.CreateSwitch(action, invalid, 11);
        for (unsigned raw = 0; raw <= 10; ++raw) {
            llvm::BasicBlock* block = llvm::BasicBlock::Create(
                _context, "action." + std::to_string(raw), _function);
            select->addCase(_builder.getInt8(raw), block);
            _builder.SetInsertPoint(block);
            if (!EmitActionKind(static_cast<AsyncAction>(raw), index, outcome))
                return false;
        }
        _builder.SetInsertPoint(invalid);
        _builder.CreateRet(_builder.getInt8(0));
        return true;
    }

    bool EmitActionKind(AsyncAction action, llvm::Value* index,
                        llvm::Value* outcome) {
        std::vector<std::size_t> states;
        for (std::size_t state = 0; state < _entry->plan->states.size();
             ++state) {
            const AsyncStateKind kind = _entry->plan->states[state].kind;
            const bool operationAction =
                action == AsyncAction::Reserve ||
                action == AsyncAction::Initialize ||
                action == AsyncAction::Poll || action == AsyncAction::Finish ||
                action == AsyncAction::CancelOperation ||
                action == AsyncAction::RetireOperation;
            const bool registration =
                _entry->machine.Instructions()[state].kind ==
                lowering::AsyncInstructionKind::RegisterDefer;
            const bool supports = kind == AsyncStateKind::DirectAwait ||
                                  ((action == AsyncAction::Reserve ||
                                    action == AsyncAction::Initialize ||
                                    action == AsyncAction::Finish) &&
                                   kind == AsyncStateKind::CreateChild) ||
                                  ((action == AsyncAction::Initialize ||
                                    action == AsyncAction::Poll ||
                                    action == AsyncAction::Finish) &&
                                   kind == AsyncStateKind::ChildAwait) ||
                                  ((action == AsyncAction::Reserve ||
                                    action == AsyncAction::Initialize) &&
                                   registration);
            if (operationAction && supports)
                states.push_back(state);
            else if (action == AsyncAction::Flow &&
                     _entry->machine.Instructions()[state].flowAction)
                states.push_back(state);
        }
        if (action == AsyncAction::Destroy) {
            llvm::BasicBlock* invalid = llvm::BasicBlock::Create(
                _context, "destroy.invalid", _function);
            auto* select = _builder.CreateSwitch(index, invalid);
            for (std::size_t slot = 0; slot < _entry->plan->slots.size();
                 ++slot) {
                const SemanticType type = _entry->plan->slots[slot].type;
                if (!type.OwnsStorage() ||
                    !_entry->generated.frame.slotOffsets[slot])
                    continue;
                llvm::BasicBlock* block = llvm::BasicBlock::Create(
                    _context, "destroy." + std::to_string(slot), _function);
                select->addCase(_builder.getInt64(slot), block);
                _builder.SetInsertPoint(block);
                DestroyAt(Slot(slot), type);
                _builder.CreateRet(_builder.getInt8(1));
            }
            _builder.SetInsertPoint(invalid);
            _builder.CreateRet(_builder.getInt8(0));
            return true;
        }
        if (action == AsyncAction::RunDefers) {
            return EmitRunDefers(index);
        }
        if (action == AsyncAction::CancelChildren ||
            action == AsyncAction::DrainChildren) {
            llvm::BasicBlock* invalid =
                llvm::BasicBlock::Create(_context, "scope.invalid", _function);
            auto* select = _builder.CreateSwitch(index, invalid,
                                                 _entry->plan->scopes.size());
            for (std::size_t scope = 0; scope < _entry->plan->scopes.size();
                 ++scope) {
                if (!_entry->plan->scopes[scope].children) continue;
                llvm::BasicBlock* block = llvm::BasicBlock::Create(
                    _context, "scope." + std::to_string(scope), _function);
                select->addCase(_builder.getInt64(scope), block);
                _builder.SetInsertPoint(block);
                llvm::Value* storage =
                    Slot(*_entry->plan->scopes[scope].children);
                if (action == AsyncAction::CancelChildren) {
                    auto* type = llvm::FunctionType::get(
                        _builder.getVoidTy(), {_builder.getPtrTy()}, false);
                    _builder.CreateCall(
                        Runtime("flecha_async_child_scope_cancel", type),
                        {storage});
                    _builder.CreateRet(_builder.getInt8(1));
                } else {
                    auto* type = llvm::FunctionType::get(
                        _builder.getInt1Ty(),
                        {_builder.getPtrTy(), _builder.getPtrTy()}, false);
                    llvm::Value* drained = _builder.CreateCall(
                        Runtime("flecha_async_child_scope_drain", type),
                        {storage, _contextValue});
                    _builder.CreateRet(
                        _builder.CreateZExt(drained, _builder.getInt8Ty()));
                }
            }
            _builder.SetInsertPoint(invalid);
            _builder.CreateRet(_builder.getInt8(0));
            return true;
        }
        if (states.empty()) {
            _builder.CreateRet(_builder.getInt8(1));
            return true;
        }
        llvm::BasicBlock* invalid =
            llvm::BasicBlock::Create(_context, "state.invalid", _function);
        auto* select = _builder.CreateSwitch(index, invalid, states.size());
        for (std::size_t state : states) {
            llvm::BasicBlock* block = llvm::BasicBlock::Create(
                _context, "state." + std::to_string(state), _function);
            select->addCase(_builder.getInt64(state), block);
            _builder.SetInsertPoint(block);
            _actionResult = nullptr;
            _flowResult = nullptr;
            bool emitted = true;
            switch (action) {
                case AsyncAction::Flow: {
                    const std::uint8_t result = Flow(state);
                    if (!result) return false;
                    _builder.CreateRet(result == 2 ? _flowResult
                                                   : _builder.getInt8(1));
                    break;
                }
                case AsyncAction::Reserve:
                    emitted = EmitReserve(state);
                    if (emitted) _builder.CreateRet(_actionResult);
                    break;
                case AsyncAction::Initialize:
                    emitted = EmitInitialize(state);
                    if (emitted) _builder.CreateRet(_builder.getInt8(1));
                    break;
                case AsyncAction::Poll:
                    emitted = EmitPoll(state, false);
                    if (emitted) _builder.CreateRet(_actionResult);
                    break;
                case AsyncAction::Finish:
                    emitted = EmitFinish(state, outcome);
                    if (emitted) _builder.CreateRet(_builder.getInt8(1));
                    break;
                case AsyncAction::CancelOperation:
                    emitted = EmitPoll(state, true);
                    if (emitted) _builder.CreateRet(_actionResult);
                    break;
                case AsyncAction::RetireOperation: {
                    llvm::Value* operation = OperationStorage(state);
                    auto* releaseType = llvm::FunctionType::get(
                        _builder.getInt1Ty(), {_builder.getPtrTy()}, false);
                    _builder.CreateCall(
                        Runtime("flecha_async_computation_release",
                                releaseType),
                        {operation});
                    _builder.CreateRet(_builder.getInt8(1));
                    break;
                }
                default:
                    _builder.CreateRet(_builder.getInt8(1));
                    break;
            }
            if (!emitted) return false;
        }
        _builder.SetInsertPoint(invalid);
        _builder.CreateRet(_builder.getInt8(0));
        return true;
    }

    bool EmitInitializeWrapper() {
        llvm::Type* pointer = _builder.getPtrTy();
        auto* type = llvm::FunctionType::get(
            _builder.getVoidTy(), {pointer, pointer, pointer}, false);
        _function = _module.getFunction(_entry->generated.name + "_initialize");
        _function->addFnAttr(llvm::Attribute::NoUnwind);
        _frame = _function->getArg(0);
        llvm::Value* arguments = _function->getArg(1);
        llvm::Value* resource = _function->getArg(2);
        _builder.SetInsertPoint(
            llvm::BasicBlock::Create(_context, "entry", _function));
        _builder.CreateMemSet(_frame, _builder.getInt8(0),
                              _builder.getInt64(_entry->generated.frame.size),
                              llvm::Align(_entry->generated.frame.alignment));
        _builder.CreateStore(
            resource, BytePointer(_entry->generated.frame.resourceOffset));
        auto* constructType = llvm::FunctionType::get(
            _builder.getVoidTy(), {_builder.getPtrTy()}, false);
        for (std::size_t operation = 0;
             operation < _entry->generated.frame.operationOffsets.size();
             ++operation)
            if (_entry->plan->slots[_entry->machine.OperationSlots()[operation]]
                    .kind == AsyncSlotKind::Computation)
                _builder.CreateCall(
                    Runtime("flecha_async_computation_construct",
                            constructType),
                    {BytePointer(
                        _entry->generated.frame.operationOffsets[operation])});
        for (const semantic::AsyncScopePlan& scope : _entry->plan->scopes)
            if (scope.children) {
                auto* scopeType = llvm::FunctionType::get(
                    _builder.getVoidTy(), {_builder.getPtrTy()}, false);
                _builder.CreateCall(
                    Runtime("flecha_async_child_scope_construct", scopeType),
                    {Slot(*scope.children)});
            }
        for (const semantic::AsyncScopePlan& scope : _entry->plan->scopes)
            if (scope.deferredStack) {
                auto* scopeType = llvm::FunctionType::get(
                    _builder.getVoidTy(), {_builder.getPtrTy()}, false);
                _builder.CreateCall(
                    Runtime("flecha_async_defer_scope_construct", scopeType),
                    {Slot(*scope.deferredStack)});
            }
        llvm::Function* control =
            _module.getFunction(_entry->generated.name + "_control_initialize");
        if (!control)
            return Fail("missing generated async control initializer");
        _builder.CreateCall(
            control,
            {BytePointer(_entry->generated.frame.controlOffset),
             BytePointer(_entry->generated.frame.initializedOffset),
             BytePointer(_entry->generated.frame.activeOperationsOffset)});
        llvm::StructType* argumentType = ArgumentType(*_entry);
        for (std::size_t index = 0; index < _entry->machine.Parameters().size();
             ++index) {
            const std::size_t slot = _entry->machine.Parameters()[index];
            llvm::Type* valueType = _types.Map(_entry->plan->slots[slot].type);
            llvm::Value* source =
                _builder.CreateStructGEP(argumentType, arguments, index);
            _builder.CreateStore(_builder.CreateLoad(valueType, source),
                                 Slot(slot));
        }
        _builder.CreateRetVoid();
        return true;
    }

    llvm::StructType* ArgumentType(const FunctionEntry& entry) {
        std::vector<llvm::Type*> fields;
        for (SemanticType type : entry.symbol->ParameterTypes())
            fields.push_back(_types.Map(type));
        return llvm::StructType::get(_context, fields);
    }

    bool EmitPollWrapper() {
        llvm::Type* pointer = _builder.getPtrTy();
        auto* type = llvm::FunctionType::get(_builder.getInt32Ty(),
                                             {pointer, pointer}, false);
        _function = _module.getFunction(_entry->generated.name + "_frame_poll");
        _function->addFnAttr(llvm::Attribute::NoUnwind);
        _frame = _function->getArg(0);
        _contextValue = _function->getArg(1);
        _builder.SetInsertPoint(
            llvm::BasicBlock::Create(_context, "entry", _function));
        auto* cancelType = llvm::FunctionType::get(
            _builder.getInt1Ty(), {_builder.getPtrTy()}, false);
        llvm::Value* cancelled = _builder.CreateCall(
            Runtime("flecha_async_context_cancelled", cancelType),
            {_contextValue});
        auto* readyType = llvm::FunctionType::get(
            _builder.getInt1Ty(), {_builder.getPtrTy(), _builder.getPtrTy()},
            false);
        llvm::Value* ready = _builder.CreateCall(
            Runtime("flecha_async_context_ready", readyType),
            {_contextValue, _frame});
        llvm::BasicBlock* poll =
            llvm::BasicBlock::Create(_context, "poll", _function);
        llvm::BasicBlock* pending =
            llvm::BasicBlock::Create(_context, "external.pending", _function);
        _builder.CreateCondBr(_builder.CreateOr(cancelled, ready), poll,
                              pending);
        _builder.SetInsertPoint(pending);
        _builder.CreateRet(_builder.getInt32(0));
        _builder.SetInsertPoint(poll);
        llvm::Function* control =
            _module.getFunction(_entry->generated.name + "_control_poll");
        if (!control) return Fail("missing generated async control poll");
        llvm::Value* result = _builder.CreateCall(
            control, {BytePointer(_entry->generated.frame.controlOffset),
                      _frame, _contextValue,
                      _builder.CreateZExt(cancelled, _builder.getInt8Ty()),
                      _module.getFunction(_entry->generated.name + "_action")});
        _builder.CreateRet(_builder.CreateZExt(result, _builder.getInt32Ty()));
        return true;
    }

    bool EmitTakeWrapper() {
        if (!_entry->plan->returnValue) return true;
        llvm::Type* pointer = _builder.getPtrTy();
        auto* type = llvm::FunctionType::get(_builder.getVoidTy(),
                                             {pointer, pointer}, false);
        _function = _module.getFunction(_entry->generated.name + "_take");
        _frame = _function->getArg(0);
        _builder.SetInsertPoint(
            llvm::BasicBlock::Create(_context, "entry", _function));
        const std::size_t slot = *_entry->plan->returnValue;
        llvm::Type* valueType = _types.Map(_entry->plan->slots[slot].type);
        _builder.CreateStore(_builder.CreateLoad(valueType, Slot(slot)),
                             _function->getArg(1));
        _builder.CreateStore(_builder.getInt8(0), Guard(slot));
        _builder.CreateRetVoid();
        return true;
    }

    bool EmitDestroyWrapper() {
        auto* type = llvm::FunctionType::get(_builder.getVoidTy(),
                                             {_builder.getPtrTy()}, false);
        _function = _module.getFunction(_entry->generated.name + "_destroy");
        _frame = _function->getArg(0);
        _builder.SetInsertPoint(
            llvm::BasicBlock::Create(_context, "entry", _function));
        for (std::size_t slot = _entry->plan->slots.size(); slot-- > 0;) {
            const SemanticType valueType = _entry->plan->slots[slot].type;
            if (!valueType.OwnsStorage() ||
                !_entry->generated.frame.slotOffsets[slot])
                continue;
            llvm::BasicBlock* destroy =
                llvm::BasicBlock::Create(_context, "slot.destroy", _function);
            llvm::BasicBlock* next =
                llvm::BasicBlock::Create(_context, "slot.done", _function);
            _builder.CreateCondBr(
                _builder.CreateICmpNE(
                    _builder.CreateLoad(_builder.getInt8Ty(), Guard(slot)),
                    _builder.getInt8(0)),
                destroy, next);
            _builder.SetInsertPoint(destroy);
            DestroyAt(Slot(slot), valueType);
            _builder.CreateStore(_builder.getInt8(0), Guard(slot));
            _builder.CreateBr(next);
            _builder.SetInsertPoint(next);
        }
        auto* destructType = llvm::FunctionType::get(
            _builder.getVoidTy(), {_builder.getPtrTy()}, false);
        for (std::size_t operation = 0;
             operation < _entry->generated.frame.operationOffsets.size();
             ++operation)
            if (_entry->plan->slots[_entry->machine.OperationSlots()[operation]]
                    .kind == AsyncSlotKind::Computation)
                _builder.CreateCall(
                    Runtime("flecha_async_computation_destruct", destructType),
                    {BytePointer(
                        _entry->generated.frame.operationOffsets[operation])});
        for (const semantic::AsyncScopePlan& scope : _entry->plan->scopes)
            if (scope.children)
                _builder.CreateCall(
                    Runtime("flecha_async_child_scope_destruct", destructType),
                    {Slot(*scope.children)});
        for (const semantic::AsyncScopePlan& scope : _entry->plan->scopes)
            if (scope.deferredStack)
                _builder.CreateCall(
                    Runtime("flecha_async_defer_scope_destruct", destructType),
                    {Slot(*scope.deferredStack)});
        _builder.CreateRetVoid();
        return true;
    }

    bool EmitFunction(FunctionEntry& entry) {
        _entry = &entry;
        _defers.clear();
        _rootExpressions.clear();
        _terminalExpressions.clear();
        for (const semantic::FlowNode& node : entry.plan->flow.nodes) {
            if (node.evaluation) _rootExpressions.insert(node.evaluation);
            if (node.condition) _terminalExpressions.insert(node.condition);
            if (node.memoryDeclaration && node.memoryDeclaration->Initializer())
                _terminalExpressions.insert(
                    node.memoryDeclaration->Initializer());
            if (node.memoryReturn && node.memoryReturn->Value())
                _terminalExpressions.insert(node.memoryReturn->Value());
            if (node.memoryDestructure)
                _terminalExpressions.insert(&node.memoryDestructure->Value());
            if (node.memoryDiscard)
                _terminalExpressions.insert(node.memoryDiscard);
        }
        return EmitAction() && EmitInitializeWrapper() && EmitPollWrapper() &&
               EmitTakeWrapper() && EmitDestroyWrapper();
    }

    llvm::LLVMContext& _context;
    const llvm::DataLayout& _dataLayout;
    const semantic::SemanticModel& _model;
    llvm::Module& _module;
    std::vector<FunctionEntry>& _entries;
    std::string& _error;
    llvm::IRBuilder<> _builder;
    TypeMapper _types;
    std::unordered_map<const semantic::FunctionSymbol*, FunctionEntry*>
        _functions;
    FunctionEntry* _entry = nullptr;
    llvm::Function* _function = nullptr;
    llvm::Value* _frame = nullptr;
    llvm::Value* _contextValue = nullptr;
    llvm::Value* _actionResult = nullptr;
    llvm::Value* _flowResult = nullptr;
    std::set<const core::Expression*> _rootExpressions;
    std::set<const core::Expression*> _terminalExpressions;
    std::unordered_map<const semantic::Symbol*, llvm::Value*> _localAddresses;
    std::unordered_map<const core::DeferStatement*, DeferredInfo> _defers;
    std::set<const semantic::FunctionSymbol*> _emittedSync;
};

bool EmitAsyncEntry(llvm::Module& module,
                    const std::vector<FunctionEntry>& entries,
                    const llvm::DataLayout& dataLayout, std::string& error) {
    const FunctionEntry* main = nullptr;
    for (const FunctionEntry& entry : entries)
        if (entry.symbol->LinkName() == "main") {
            if (main) {
                error = "an executable has more than one async main";
                return false;
            }
            main = &entry;
        }
    if (!main) {
        error =
            "an async executable requires async i32 main() or async void "
            "main() in its entry module";
        return false;
    }
    const SemanticTypeKind returnKind = main->symbol->ReturnType().Kind();
    if (!main->symbol->ParameterTypes().empty() ||
        (returnKind != SemanticTypeKind::Void &&
         returnKind != SemanticTypeKind::I32)) {
        error =
            "an async executable requires async i32 main() or async void "
            "main() in its entry module";
        return false;
    }

    llvm::LLVMContext& context = module.getContext();
    llvm::IRBuilder<> builder(context);
    llvm::Type* pointer = builder.getPtrTy();
    auto* entryType = llvm::FunctionType::get(
        builder.getInt32Ty(), {builder.getInt32Ty(), pointer}, false);
    llvm::Function* entry = llvm::Function::Create(
        entryType, llvm::Function::ExternalLinkage, "main", module);
    entry->getArg(0)->setName("argument.count");
    entry->getArg(1)->setName("argument.values");
    builder.SetInsertPoint(llvm::BasicBlock::Create(context, "entry", entry));

    auto* processArgumentsType = llvm::FunctionType::get(
        builder.getVoidTy(), {builder.getInt32Ty(), pointer}, false);
    builder.CreateCall(module.getOrInsertFunction(
                           "flecha_runtime_v1_process_arguments_initialize",
                           processArgumentsType),
                       {entry->getArg(0), entry->getArg(1)});

    llvm::Type* sizeType =
        llvm::IntegerType::get(context, dataLayout.getPointerSizeInBits());
    auto* runType =
        llvm::FunctionType::get(builder.getInt32Ty(),
                                {sizeType, sizeType, pointer, pointer, pointer,
                                 pointer, builder.getInt8Ty()},
                                false);
    llvm::Function* poll =
        module.getFunction(main->generated.name + "_frame_poll");
    llvm::Function* destroy =
        module.getFunction(main->generated.name + "_destroy");
    llvm::Function* initialize =
        module.getFunction(main->generated.name + "_initialize");
    llvm::Constant* take =
        llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(context));
    if (returnKind == SemanticTypeKind::I32)
        take = module.getFunction(main->generated.name + "_take");
    if (!poll || !destroy || !initialize ||
        (returnKind == SemanticTypeKind::I32 && take->isNullValue())) {
        error = "async main is missing generated frame callbacks";
        return false;
    }
    builder.CreateRet(builder.CreateCall(
        module.getOrInsertFunction("flecha_runtime_v1_async_main_run", runType),
        {llvm::ConstantInt::get(sizeType, main->generated.frame.size),
         llvm::ConstantInt::get(sizeType, main->generated.frame.alignment),
         poll, destroy, take, initialize,
         builder.getInt8(returnKind == SemanticTypeKind::I32 ? 1 : 0)}));
    return true;
}

}  // namespace

static std::optional<GeneratedAsyncProgram> GenerateAsyncValues(
    llvm::LLVMContext& context, const llvm::DataLayout& dataLayout,
    const semantic::SemanticModel& model,
    const diagnostics::DiagnosticBag& diagnostics, std::string moduleName,
    std::string& error, bool emitEntry) {
    error.clear();
    for (const diagnostics::Diagnostic& diagnostic : diagnostics.Diagnostics())
        if (diagnostic.severity == diagnostics::DiagnosticSeverity::Error) {
            error =
                "async value generation requires successful semantic analysis";
            return std::nullopt;
        }
    std::vector<FunctionEntry> entries;
    for (const std::unique_ptr<semantic::Symbol>& owned : model.Symbols()) {
        if (owned->Kind() != semantic::SymbolKind::Function) continue;
        const auto* symbol =
            static_cast<const semantic::FunctionSymbol*>(owned.get());
        if (!symbol->IsAsync() || !symbol->Declaration()) continue;
        const semantic::AsyncFramePlan* plan =
            model.AsyncPlanFor(*symbol->Declaration());
        if (!plan) continue;
        std::string loweringError;
        std::optional<lowering::AsyncMachine> machine =
            lowering::LowerAsyncMachine(*plan, model, diagnostics,
                                        loweringError);
        if (!machine) {
            error = loweringError;
            return std::nullopt;
        }
        FunctionEntry entry{symbol, plan, std::move(*machine), {}};
        GeneratedAsyncFunction generated;
        generated.symbol = symbol;
        generated.name =
            moduleName + "_async_" + std::to_string(entries.size());
        std::optional<AsyncConcreteFrameLayout> frame = PlanAsyncFrameLayout(
            context, dataLayout, *plan, entry.machine, error);
        if (!frame) return std::nullopt;
        generated.frame = std::move(*frame);
        std::size_t argumentCursor = 0;
        for (SemanticType parameter : symbol->ParameterTypes()) {
            TypeMapper mapper(context, dataLayout);
            llvm::Type* type = mapper.Map(parameter);
            if (!type) {
                error = "unsupported async parameter type '" +
                        std::string(parameter.Name()) + "'";
                return std::nullopt;
            }
            const std::size_t alignment =
                dataLayout.getABITypeAlign(type).value();
            argumentCursor = Align(argumentCursor, alignment);
            generated.argumentOffsets.push_back(argumentCursor);
            argumentCursor += dataLayout.getTypeAllocSize(type).getFixedValue();
            generated.argumentAlignment =
                std::max(generated.argumentAlignment, alignment);
        }
        generated.argumentSize =
            Align(argumentCursor, generated.argumentAlignment);
        entry.generated = std::move(generated);
        entries.push_back(std::move(entry));
    }
    if (entries.empty()) {
        error = "no verified async functions to generate";
        return std::nullopt;
    }
    auto module = std::make_unique<llvm::Module>(moduleName, context);
    module->setDataLayout(dataLayout);
    for (FunctionEntry& entry : entries) {
        std::unique_ptr<llvm::Module> control = GenerateAsyncMachine(
            context, entry.machine, entry.generated.name + "_control");
        control->setDataLayout(dataLayout);
        if (llvm::Linker::linkModules(*module, std::move(control))) {
            error = "failed to link generated async control module";
            return std::nullopt;
        }
    }
    Emitter emitter(context, dataLayout, model, *module, entries, error);
    if (!emitter.Emit()) return std::nullopt;
    if (emitEntry && !EmitAsyncEntry(*module, entries, dataLayout, error))
        return std::nullopt;
    std::string verification;
    llvm::raw_string_ostream output(verification);
    if (llvm::verifyModule(*module, &output)) {
        output.flush();
        error = "invalid generated async value module: " + verification;
        return std::nullopt;
    }
    GeneratedAsyncProgram result;
    result.module = std::move(module);
    for (FunctionEntry& entry : entries)
        result.functions.push_back(std::move(entry.generated));
    return result;
}

std::optional<GeneratedAsyncProgram> GenerateAsyncValuesForTesting(
    llvm::LLVMContext& context, const llvm::DataLayout& dataLayout,
    const semantic::SemanticModel& model,
    const diagnostics::DiagnosticBag& diagnostics, std::string moduleName,
    std::string& error) {
    return GenerateAsyncValues(context, dataLayout, model, diagnostics,
                               std::move(moduleName), error, false);
}

std::optional<GeneratedAsyncProgram> GenerateAsyncProgram(
    llvm::LLVMContext& context, const llvm::DataLayout& dataLayout,
    const semantic::SemanticModel& model,
    const diagnostics::DiagnosticBag& diagnostics, std::string moduleName,
    std::string& error) {
    return GenerateAsyncValues(context, dataLayout, model, diagnostics,
                               std::move(moduleName), error, true);
}

}  // namespace flecha::codegen
