#include "codegen/async_code_generator.hpp"

#include <llvm/Config/llvm-config.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/raw_ostream.h>

#include <stdexcept>

#include "runtime/async_executor.hpp"

namespace flecha::codegen {
namespace {
using Kind = lowering::AsyncInstructionKind;
using Result = runtime::async::PollResult;
static_assert(static_cast<unsigned>(Result::Pending) == 0 &&
              static_cast<unsigned>(Result::Ready) == 1 &&
              static_cast<unsigned>(Result::Cancelled) == 2 &&
              static_cast<unsigned>(Result::AllocationFailed) == 3);

class Emitter final {
   public:
    Emitter(llvm::LLVMContext& context, const lowering::AsyncMachine& machine,
            const std::string& name)
        : _machine(machine),
          _module(std::make_unique<llvm::Module>(name, context)),
          _builder(context),
          _byte(_builder.getInt8Ty()),
          _index(_builder.getInt64Ty()),
          _pointer(llvm::PointerType::getUnqual(context)) {
        _controlType = llvm::StructType::get(
            context, {_index, _byte, _byte, _pointer, _pointer});
        _actionType = llvm::FunctionType::get(
            _byte, {_pointer, _pointer, _byte, _index, _byte}, false);
        Initialize(name);
        Poll(name);
    }
    std::unique_ptr<llvm::Module> Take() { return std::move(_module); }

   private:
    llvm::Value* Field(unsigned field) {
        return _builder.CreateStructGEP(_controlType, _control, field);
    }
    llvm::Value* Flag(unsigned field, std::size_t id) {
        return _builder.CreateInBoundsGEP(
            _byte, _builder.CreateLoad(_pointer, Field(field)),
            _builder.getInt64(id));
    }
    void SetFlag(unsigned field, std::size_t id, bool value) {
        _builder.CreateStore(_builder.getInt8(value), Flag(field, id));
    }
    llvm::Value* HasFlag(unsigned field, std::size_t id) {
        return _builder.CreateICmpNE(
            _builder.CreateLoad(_byte, Flag(field, id)), _builder.getInt8(0));
    }
    llvm::BasicBlock* Block(const std::string& name) {
        return llvm::BasicBlock::Create(_module->getContext(), name, _function);
    }
    llvm::Value* Action(AsyncAction action, std::size_t index,
                        llvm::Value* outcome = nullptr) {
        return _builder.CreateCall(
            _actionType, _callback,
            {_storage, _context,
             _builder.getInt8(static_cast<unsigned>(action)),
             _builder.getInt64(index),
             outcome ? outcome : _builder.getInt8(0)});
    }
    void Go(std::size_t state) {
        _builder.CreateStore(_builder.getInt64(state), Field(0));
        _builder.CreateBr(_blocks[state]);
    }
    void Return(Result result) {
        if (result != Result::Pending)
            _builder.CreateStore(
                _builder.getInt8(static_cast<unsigned>(result)), Field(1));
        _builder.CreateRet(_builder.getInt8(static_cast<unsigned>(result)));
    }
    void Guards(const lowering::AsyncInstruction& instruction) {
        for (auto slot : instruction.resets) SetFlag(3, slot, false);
        for (auto slot : instruction.moves) SetFlag(3, slot, false);
        for (auto slot : instruction.writes) SetFlag(3, slot, true);
    }
    void Destroy(const lowering::AsyncInstruction& instruction) {
        for (auto slot : instruction.destroys) {
            auto* destroy = Block("destroy");
            auto* next = Block("destroy.next");
            _builder.CreateCondBr(HasFlag(3, slot), destroy, next);
            _builder.SetInsertPoint(destroy);
            // Clear before invoking user destruction, so the activation cannot
            // be destroyed twice. Polling reentrancy remains forbidden.
            SetFlag(3, slot, false);
            Action(AsyncAction::Destroy, slot);
            _builder.CreateBr(next);
            _builder.SetInsertPoint(next);
        }
    }
    void Initialize(const std::string& name) {
        _function = llvm::Function::Create(
            llvm::FunctionType::get(_builder.getVoidTy(),
                                    {_pointer, _pointer, _pointer}, false),
            llvm::Function::ExternalLinkage, name + "_initialize", *_module);
        _function->addFnAttr(llvm::Attribute::NoUnwind);
        _control = _function->getArg(0);
        _builder.SetInsertPoint(Block("entry"));
        _builder.CreateStore(_builder.getInt64(_machine.Entry()), Field(0));
        _builder.CreateStore(_builder.getInt8(0), Field(1));
        _builder.CreateStore(_builder.getInt8(0), Field(2));
        _builder.CreateStore(_function->getArg(1), Field(3));
        _builder.CreateStore(_function->getArg(2), Field(4));
        for (std::size_t slot = 0; slot < _machine.SlotCount(); ++slot)
            SetFlag(3, slot, false);
        for (auto slot : _machine.Parameters()) SetFlag(3, slot, true);
        for (std::size_t op = 0; op < _machine.OperationCount(); ++op)
            SetFlag(4, op, false);
        _builder.CreateRetVoid();
    }
    void Poll(const std::string& name) {
        _function = llvm::Function::Create(
            llvm::FunctionType::get(
                _byte, {_pointer, _pointer, _pointer, _byte, _pointer}, false),
            llvm::Function::ExternalLinkage, name + "_poll", *_module);
        _function->addFnAttr(llvm::Attribute::NoUnwind);
        _control = _function->getArg(0);
        _storage = _function->getArg(1);
        _context = _function->getArg(2);
        _callback = _function->getArg(4);
        _builder.SetInsertPoint(Block("entry"));
        auto* cached = Block("terminal");
        auto* start = Block("start");
        auto* dispatch = Block("dispatch");
        _trap = Block("invalid.backend.protocol");
        for (std::size_t id = 0; id < _machine.Instructions().size(); ++id)
            _blocks.push_back(Block("state." + std::to_string(id)));
        auto* terminal = _builder.CreateLoad(_byte, Field(1));
        _builder.CreateCondBr(
            _builder.CreateICmpNE(terminal, _builder.getInt8(0)), cached,
            start);
        _builder.SetInsertPoint(cached);
        _builder.CreateRet(terminal);
        _builder.SetInsertPoint(start);
        auto* wasStarted = _builder.CreateLoad(_byte, Field(2));
        _builder.CreateStore(_builder.getInt8(1), Field(2));
        _cancel =
            _builder.CreateICmpNE(_function->getArg(3), _builder.getInt8(0));
        auto* initialCancel = Block("initial.cancel");
        _builder.CreateCondBr(
            _builder.CreateAnd(_cancel, _builder.CreateICmpEQ(
                                            wasStarted, _builder.getInt8(0))),
            initialCancel, dispatch);
        _builder.SetInsertPoint(initialCancel);
        Go(_machine.InitialCancellation());
        _builder.SetInsertPoint(dispatch);
        auto* select = _builder.CreateSwitch(
            _builder.CreateLoad(_index, Field(0)), _trap, _blocks.size());
        for (std::size_t id = 0; id < _blocks.size(); ++id)
            select->addCase(_builder.getInt64(id), _blocks[id]);
        _builder.SetInsertPoint(_trap);
#if LLVM_VERSION_MAJOR >= 20
        _builder.CreateCall(llvm::Intrinsic::getOrInsertDeclaration(
            _module.get(), llvm::Intrinsic::trap));
#else
        _builder.CreateCall(llvm::Intrinsic::getDeclaration(
            _module.get(), llvm::Intrinsic::trap));
#endif
        _builder.CreateUnreachable();
        for (std::size_t id = 0; id < _blocks.size(); ++id) Emit(id);
    }
    void Emit(std::size_t id) {
        const auto& instruction = _machine.Instructions()[id];
        _builder.SetInsertPoint(_blocks[id]);
        if (instruction.cancellation) {
            auto* resume = Block("not.cancelled");
            auto* cancel = Block("cancel");
            _builder.CreateCondBr(_cancel, cancel, resume);
            _builder.SetInsertPoint(cancel);
            Go(*instruction.cancellation);
            _builder.SetInsertPoint(resume);
        }
        const auto source = instruction.sourceState.value_or(id);
        if (instruction.activation) {
            auto* active = Block("scope.active");
            auto* inactive = Block("scope.inactive");
            _builder.CreateCondBr(HasFlag(3, *instruction.activation), active,
                                  inactive);
            _builder.SetInsertPoint(inactive);
            Guards(instruction);
            Go(instruction.successors[0]);
            _builder.SetInsertPoint(active);
        }
        switch (instruction.kind) {
            case Kind::Complete:
            case Kind::Failed:
            case Kind::Cancelled:
                Destroy(instruction);
                Guards(instruction);
                Return(instruction.kind == Kind::Complete ? Result::Ready
                       : instruction.kind == Kind::Failed
                           ? Result::AllocationFailed
                           : Result::Cancelled);
                return;
            case Kind::CreateChild:
            case Kind::Await:
                Operation(instruction, id, source);
                return;
            case Kind::RegisterDefer:
                RegisterDefer(instruction, source);
                return;
            case Kind::CancelOperation: {
                auto* poll = Block("retire.poll");
                auto* done = Block("retire.done");
                _builder.CreateCondBr(HasFlag(4, *instruction.operation), poll,
                                      done);
                _builder.SetInsertPoint(poll);
                auto* result = Action(AsyncAction::CancelOperation, source);
                auto* pending = Block("retire.pending");
                auto* terminal = Block("retire.terminal");
                auto* select = _builder.CreateSwitch(result, _trap, 4);
                select->addCase(_builder.getInt8(0), pending);
                for (unsigned value : {1u, 2u, 3u})
                    select->addCase(_builder.getInt8(value), terminal);
                _builder.SetInsertPoint(pending);
                _builder.CreateStore(_builder.getInt64(id), Field(0));
                Return(Result::Pending);
                _builder.SetInsertPoint(terminal);
                Action(AsyncAction::RetireOperation, source, result);
                SetFlag(4, *instruction.operation, false);
                _builder.CreateBr(done);
                _builder.SetInsertPoint(done);
                Go(instruction.successors[0]);
                return;
            }
            case Kind::DrainChildren: {
                auto* result =
                    Action(AsyncAction::DrainChildren, *instruction.scope);
                auto* pending = Block("drain.pending");
                auto* retired = Block("drain.retired");
                auto* select = _builder.CreateSwitch(result, _trap, 2);
                select->addCase(_builder.getInt8(0), pending);
                select->addCase(_builder.getInt8(1), retired);
                _builder.SetInsertPoint(pending);
                _builder.CreateStore(_builder.getInt64(id), Field(0));
                Return(Result::Pending);
                _builder.SetInsertPoint(retired);
                break;
            }
            case Kind::CancelChildren:
                Action(AsyncAction::CancelChildren, *instruction.scope);
                break;
            case Kind::RunDefers:
                Action(AsyncAction::RunDefers, *instruction.scope);
                break;
            case Kind::CleanupScope:
                Destroy(instruction);
                break;
            case Kind::Flow:
            case Kind::Branch: {
                llvm::Value* condition = instruction.flowAction
                                             ? Action(AsyncAction::Flow, source)
                                             : _builder.getInt8(1);
                Guards(instruction);
                if (instruction.kind == Kind::Branch) {
                    auto* yes = Block("branch.true");
                    auto* no = Block("branch.false");
                    auto* select = _builder.CreateSwitch(condition, _trap, 2);
                    select->addCase(_builder.getInt8(1), yes);
                    select->addCase(_builder.getInt8(0), no);
                    _builder.SetInsertPoint(yes);
                    Go(instruction.successors[0]);
                    _builder.SetInsertPoint(no);
                    Go(instruction.successors[1]);
                } else
                    Go(instruction.successors[0]);
                return;
            }
        }
        Guards(instruction);
        Go(instruction.successors[0]);
    }
    void Operation(const lowering::AsyncInstruction& instruction,
                   std::size_t id, std::size_t source) {
        auto* initialize = Block("operation.initialize");
        auto* ready = Block("operation.ready");
        auto* failed = Block("operation.failed");
        auto* begin = Block("operation.begin");
        auto* poll = Block("operation.poll");
        if (instruction.operation)
            _builder.CreateCondBr(HasFlag(4, *instruction.operation), poll,
                                  begin);
        else
            _builder.CreateBr(begin);
        _builder.SetInsertPoint(begin);
        if (instruction.reservation) {
            auto* reserved = Action(AsyncAction::Reserve, source);
            auto* select = _builder.CreateSwitch(reserved, _trap, 2);
            select->addCase(_builder.getInt8(1), initialize);
            select->addCase(_builder.getInt8(0), failed);
        } else
            _builder.CreateBr(initialize);
        _builder.SetInsertPoint(initialize);
        Action(AsyncAction::Initialize, source);
        for (auto slot : instruction.argumentMoves) SetFlag(3, slot, false);
        if (instruction.operation) SetFlag(4, *instruction.operation, true);
        _builder.CreateBr(instruction.kind == Kind::CreateChild ? ready : poll);
        _builder.SetInsertPoint(failed);
        Action(AsyncAction::Finish, source, _builder.getInt8(3));
        Go(instruction.successors[1]);
        _builder.SetInsertPoint(poll);
        if (instruction.kind == Kind::CreateChild) {
            _builder.CreateUnreachable();
        } else {
            auto* result = Action(AsyncAction::Poll, source);
            auto* pending = Block("operation.pending");
            auto* failure = Block("operation.failure");
            auto* select = _builder.CreateSwitch(result, _trap, 4);
            select->addCase(_builder.getInt8(0), pending);
            select->addCase(_builder.getInt8(1), ready);
            select->addCase(_builder.getInt8(2), failure);
            select->addCase(_builder.getInt8(3), failure);
            _builder.SetInsertPoint(pending);
            _builder.CreateStore(_builder.getInt64(id), Field(0));
            Return(Result::Pending);
            _builder.SetInsertPoint(failure);
            Action(AsyncAction::Finish, source, result);
            SetFlag(4, *instruction.operation, false);
            Go(instruction.successors[1]);
        }
        _builder.SetInsertPoint(ready);
        Action(AsyncAction::Finish, source, _builder.getInt8(1));
        if (instruction.operation) SetFlag(4, *instruction.operation, false);
        Go(instruction.successors[0]);
    }

    void RegisterDefer(const lowering::AsyncInstruction& instruction,
                       std::size_t source) {
        auto* initialize = Block("defer.initialize");
        auto* failed = Block("defer.allocation-failed");
        auto* reserved = Action(AsyncAction::Reserve, source);
        auto* select = _builder.CreateSwitch(reserved, _trap, 2);
        select->addCase(_builder.getInt8(1), initialize);
        select->addCase(_builder.getInt8(0), failed);
        _builder.SetInsertPoint(failed);
        Go(instruction.successors[1]);
        _builder.SetInsertPoint(initialize);
        Action(AsyncAction::Initialize, source);
        for (auto slot : instruction.argumentMoves) SetFlag(3, slot, false);
        Guards(instruction);
        Go(instruction.successors[0]);
    }

    const lowering::AsyncMachine& _machine;
    std::unique_ptr<llvm::Module> _module;
    llvm::IRBuilder<> _builder;
    llvm::Type *_byte, *_index, *_pointer;
    llvm::StructType* _controlType;
    llvm::FunctionType* _actionType;
    llvm::Function* _function = nullptr;
    llvm::Value *_control = nullptr, *_storage = nullptr, *_context = nullptr;
    llvm::Value *_callback = nullptr, *_cancel = nullptr;
    llvm::BasicBlock* _trap = nullptr;
    std::vector<llvm::BasicBlock*> _blocks;
};
}  // namespace

std::unique_ptr<llvm::Module> GenerateAsyncMachine(
    llvm::LLVMContext& context, const lowering::AsyncMachine& machine,
    const std::string& name) {
    Emitter emitter(context, machine, name);
    auto module = emitter.Take();
    std::string error;
    llvm::raw_string_ostream stream(error);
    if (llvm::verifyModule(*module, &stream))
        throw std::logic_error("invalid async control IR: " + error);
    return module;
}
}  // namespace flecha::codegen
