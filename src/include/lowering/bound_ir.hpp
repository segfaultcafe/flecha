#ifndef FLECHA_LOWERING_BOUND_IR_HPP
#define FLECHA_LOWERING_BOUND_IR_HPP

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/token_type.hpp"
#include "semantic/symbol.hpp"
#include "source/source_range.hpp"

namespace flecha::lowering {

enum class BoundNodeKind {
    BlockStatement,
    VariableDeclarationStatement,
    ExpressionStatement,
    ReturnStatement,
    DeferStatement,
    IfStatement,
    WhileStatement,
    ProceedStatement,
    BreakStatement,
    JumpToStatement,
    PositionStatement,
    BuildExpression,
    FieldExpression,
    DefaultExpression,
    TupleExpression,
    DestructureStatement,
    ArrayExpression,
    MatchExpression,
    SliceExpression,
    ArrayElementExpression,
    ArrayLengthExpression,
    SliceDataExpression,
    LiteralExpression,
    DirectExpression,
    MoveExpression,
    VariableExpression,
    DereferenceExpression,
    AddressExpression,
    FunctionExpression,
    UnaryExpression,
    IncrementExpression,
    BinaryExpression,
    IntegerLimitExpression,
    FloatingConstantExpression,
    OverflowingArithmeticExpression,
    AssignmentExpression,
    CallExpression,
    IndirectCallExpression,
    AllocationExpression,
    StringViewOperationExpression,
    CastExpression,
};

enum class StringViewOperation {
    Length,
    At,
    Substring,
    ToString,
    View,
    Copy,
    Data,
};

class BoundNode {
   public:
    explicit BoundNode(source::SourceRange range) : _range(range) {}
    virtual ~BoundNode() = default;
    BoundNode(const BoundNode&) = delete;
    BoundNode& operator=(const BoundNode&) = delete;
    BoundNode(BoundNode&&) = default;
    BoundNode& operator=(BoundNode&&) = default;

    [[nodiscard]] virtual BoundNodeKind Kind() const = 0;
    [[nodiscard]] const source::SourceRange& Range() const { return _range; }

   private:
    source::SourceRange _range;
};

class BoundExpression : public BoundNode {
   public:
    BoundExpression(source::SourceRange range, semantic::SemanticType type)
        : BoundNode(range), _type(type) {}
    [[nodiscard]] semantic::SemanticType Type() const { return _type; }

   private:
    semantic::SemanticType _type;
};

class BoundStatement : public BoundNode {
   public:
    using BoundNode::BoundNode;
};

class BoundLiteralExpression final : public BoundExpression {
   public:
    BoundLiteralExpression(source::SourceRange range,
                           semantic::SemanticType type, std::string value,
                           bool isZeroValue = false)
        : BoundExpression(range, type),
          _value(std::move(value)),
          _isZeroValue(isZeroValue) {}
    [[nodiscard]] BoundNodeKind Kind() const override {
        return BoundNodeKind::LiteralExpression;
    }
    [[nodiscard]] const std::string& Value() const { return _value; }
    [[nodiscard]] bool IsZeroValue() const { return _isZeroValue; }

   private:
    std::string _value;
    bool _isZeroValue;
};

// A storage location can be loaded, stored, or addressed without evaluating it
// twice.
class BoundPlaceExpression : public BoundExpression {
   public:
    using BoundExpression::BoundExpression;
};

class BoundDefaultExpression final : public BoundExpression {
   public:
    BoundDefaultExpression(source::SourceRange range,
                           semantic::SemanticType type,
                           bool semanticZero = false)
        : BoundExpression(range, type), _semanticZero(semanticZero) {}
    BoundNodeKind Kind() const override {
        return BoundNodeKind::DefaultExpression;
    }
    bool IsSemanticZero() const { return _semanticZero; }

   private:
    bool _semanticZero;
};
class BoundFieldExpression final : public BoundPlaceExpression {
   public:
    BoundFieldExpression(source::SourceRange range, semantic::SemanticType type,
                         std::unique_ptr<BoundExpression> receiver,
                         std::size_t index, bool indirect)
        : BoundPlaceExpression(range, type),
          _receiver(std::move(receiver)),
          _index(index),
          _indirect(indirect) {}
    BoundNodeKind Kind() const override {
        return BoundNodeKind::FieldExpression;
    }
    const BoundExpression& Receiver() const { return *_receiver; }
    std::size_t Index() const { return _index; }
    bool Indirect() const { return _indirect; }

   private:
    std::unique_ptr<BoundExpression> _receiver;
    std::size_t _index;
    bool _indirect;
};

class BoundTupleExpression final : public BoundExpression {
   public:
    BoundTupleExpression(source::SourceRange range, semantic::SemanticType type,
                         std::vector<std::unique_ptr<BoundExpression>> elements)
        : BoundExpression(range, type), _elements(std::move(elements)) {}
    [[nodiscard]] BoundNodeKind Kind() const override {
        return BoundNodeKind::TupleExpression;
    }
    [[nodiscard]] const auto& Elements() const { return _elements; }

   private:
    std::vector<std::unique_ptr<BoundExpression>> _elements;
};

struct BoundDestructureBinding {
    const semantic::VariableSymbol* variable;
    bool declaration;
};
class BoundDestructureStatement final : public BoundStatement {
   public:
    BoundDestructureStatement(source::SourceRange range,
                              std::vector<BoundDestructureBinding> bindings,
                              std::unique_ptr<BoundExpression> value)
        : BoundStatement(range),
          _bindings(std::move(bindings)),
          _value(std::move(value)) {}
    [[nodiscard]] BoundNodeKind Kind() const override {
        return BoundNodeKind::DestructureStatement;
    }
    [[nodiscard]] const auto& Bindings() const { return _bindings; }
    [[nodiscard]] const BoundExpression& Value() const { return *_value; }

   private:
    std::vector<BoundDestructureBinding> _bindings;
    std::unique_ptr<BoundExpression> _value;
};

class BoundArrayExpression final : public BoundExpression {
   public:
    BoundArrayExpression(source::SourceRange range, semantic::SemanticType type,
                         std::vector<std::unique_ptr<BoundExpression>> elements,
                         bool repeat)
        : BoundExpression(range, type),
          _elements(std::move(elements)),
          _repeat(repeat) {}
    [[nodiscard]] BoundNodeKind Kind() const override {
        return BoundNodeKind::ArrayExpression;
    }
    [[nodiscard]] const std::vector<std::unique_ptr<BoundExpression>>&
    Elements() const {
        return _elements;
    }
    [[nodiscard]] bool IsRepeat() const { return _repeat; }

   private:
    std::vector<std::unique_ptr<BoundExpression>> _elements;
    bool _repeat;
};

struct BoundMatchArm final {
    std::optional<std::uint64_t> discriminant;
    std::unique_ptr<BoundExpression> value;
};

class BoundMatchExpression final : public BoundExpression {
   public:
    BoundMatchExpression(source::SourceRange range, semantic::SemanticType type,
                         std::unique_ptr<BoundExpression> matched,
                         std::vector<BoundMatchArm> arms)
        : BoundExpression(range, type),
          _matched(std::move(matched)),
          _arms(std::move(arms)) {}
    [[nodiscard]] BoundNodeKind Kind() const override {
        return BoundNodeKind::MatchExpression;
    }
    [[nodiscard]] const BoundExpression& Matched() const { return *_matched; }
    [[nodiscard]] const std::vector<BoundMatchArm>& Arms() const {
        return _arms;
    }

   private:
    std::unique_ptr<BoundExpression> _matched;
    std::vector<BoundMatchArm> _arms;
};

class BoundArrayElementExpression final : public BoundPlaceExpression {
   public:
    BoundArrayElementExpression(source::SourceRange range,
                                std::unique_ptr<BoundExpression> receiver,
                                std::unique_ptr<BoundExpression> index,
                                bool fromBack = false)
        : BoundPlaceExpression(range,
                               receiver->Type().Target()->WithoutConst()),
          _receiver(std::move(receiver)),
          _index(std::move(index)),
          _fromBack(fromBack) {}
    [[nodiscard]] BoundNodeKind Kind() const override {
        return BoundNodeKind::ArrayElementExpression;
    }
    [[nodiscard]] const BoundExpression& Receiver() const { return *_receiver; }
    [[nodiscard]] const BoundExpression& Index() const { return *_index; }
    [[nodiscard]] bool HasIndex() const { return _index != nullptr; }
    [[nodiscard]] bool FromBack() const { return _fromBack; }

   private:
    std::unique_ptr<BoundExpression> _receiver;
    std::unique_ptr<BoundExpression> _index;
    bool _fromBack;
};

class BoundSliceExpression final : public BoundExpression {
   public:
    BoundSliceExpression(source::SourceRange range, semantic::SemanticType type,
                         std::unique_ptr<BoundExpression> receiver,
                         std::unique_ptr<BoundExpression> start,
                         std::unique_ptr<BoundExpression> end)
        : BoundExpression(range, type),
          _receiver(std::move(receiver)),
          _start(std::move(start)),
          _end(std::move(end)) {}
    [[nodiscard]] BoundNodeKind Kind() const override {
        return BoundNodeKind::SliceExpression;
    }
    [[nodiscard]] const BoundExpression& Receiver() const { return *_receiver; }
    [[nodiscard]] const BoundExpression* Start() const { return _start.get(); }
    [[nodiscard]] const BoundExpression* End() const { return _end.get(); }

   private:
    std::unique_ptr<BoundExpression> _receiver;
    std::unique_ptr<BoundExpression> _start;
    std::unique_ptr<BoundExpression> _end;
};

class BoundArrayLengthExpression final : public BoundExpression {
   public:
    BoundArrayLengthExpression(source::SourceRange range,
                               std::unique_ptr<BoundExpression> receiver)
        : BoundExpression(
              range, semantic::SemanticType(semantic::SemanticTypeKind::USize)),
          _receiver(std::move(receiver)) {}
    [[nodiscard]] BoundNodeKind Kind() const override {
        return BoundNodeKind::ArrayLengthExpression;
    }
    [[nodiscard]] const BoundExpression& Receiver() const { return *_receiver; }

   private:
    std::unique_ptr<BoundExpression> _receiver;
};

class BoundSliceDataExpression final : public BoundExpression {
   public:
    BoundSliceDataExpression(source::SourceRange range,
                             semantic::SemanticType type,
                             std::unique_ptr<BoundExpression> receiver)
        : BoundExpression(range, type), _receiver(std::move(receiver)) {}
    [[nodiscard]] BoundNodeKind Kind() const override {
        return BoundNodeKind::SliceDataExpression;
    }
    [[nodiscard]] const BoundExpression& Receiver() const { return *_receiver; }

   private:
    std::unique_ptr<BoundExpression> _receiver;
};

class BoundDereferenceExpression final : public BoundPlaceExpression {
   public:
    BoundDereferenceExpression(source::SourceRange range,
                               std::unique_ptr<BoundExpression> pointer)
        : BoundPlaceExpression(range, pointer->Type().Target()->WithoutConst()),
          _pointer(std::move(pointer)) {}
    [[nodiscard]] BoundNodeKind Kind() const override {
        return BoundNodeKind::DereferenceExpression;
    }
    [[nodiscard]] const BoundExpression& Pointer() const { return *_pointer; }

   private:
    std::unique_ptr<BoundExpression> _pointer;
};

class BoundAddressExpression final : public BoundExpression {
   public:
    BoundAddressExpression(source::SourceRange range,
                           semantic::SemanticType type,
                           std::unique_ptr<BoundPlaceExpression> place)
        : BoundExpression(range, type), _place(std::move(place)) {}
    [[nodiscard]] BoundNodeKind Kind() const override {
        return BoundNodeKind::AddressExpression;
    }
    [[nodiscard]] const BoundPlaceExpression& Place() const { return *_place; }

   private:
    std::unique_ptr<BoundPlaceExpression> _place;
};

class BoundVariableExpression final : public BoundPlaceExpression {
   public:
    BoundVariableExpression(source::SourceRange range,
                            const semantic::VariableSymbol& symbol)
        : BoundPlaceExpression(range, symbol.Type().WithoutConst()),
          _symbol(symbol) {}
    [[nodiscard]] BoundNodeKind Kind() const override {
        return BoundNodeKind::VariableExpression;
    }
    [[nodiscard]] const semantic::VariableSymbol& Symbol() const {
        return _symbol;
    }

   private:
    const semantic::VariableSymbol& _symbol;
};

class BoundMoveExpression final : public BoundExpression {
   public:
    BoundMoveExpression(source::SourceRange range,
                        std::unique_ptr<BoundPlaceExpression> source)
        : BoundExpression(range, source->Type()), _source(std::move(source)) {}
    [[nodiscard]] BoundNodeKind Kind() const override {
        return BoundNodeKind::MoveExpression;
    }
    [[nodiscard]] const BoundPlaceExpression& Source() const {
        return *_source;
    }

   private:
    std::unique_ptr<BoundPlaceExpression> _source;
};

class BoundDirectExpression final : public BoundExpression {
   public:
    BoundDirectExpression(source::SourceRange range,
                          std::unique_ptr<BoundExpression> value)
        : BoundExpression(range, value->Type()), _value(std::move(value)) {}
    [[nodiscard]] BoundNodeKind Kind() const override {
        return BoundNodeKind::DirectExpression;
    }
    [[nodiscard]] const BoundExpression& Value() const { return *_value; }

   private:
    std::unique_ptr<BoundExpression> _value;
};

struct BoundClosureCapture final {
    core::AnonymousFunctionExpression::CaptureMode mode;
    const semantic::VariableSymbol* variable;
    std::unique_ptr<BoundExpression> value;
};

struct BoundFunctionCapture final {
    core::AnonymousFunctionExpression::CaptureMode mode;
    const semantic::VariableSymbol* variable;
};

class BoundFunctionExpression final : public BoundExpression {
   public:
    BoundFunctionExpression(source::SourceRange range,
                            const semantic::FunctionSymbol& symbol,
                            std::vector<BoundClosureCapture> captures = {})
        : BoundExpression(range, symbol.Type().WithoutConst()),
          _symbol(symbol),
          _captures(std::move(captures)) {}
    [[nodiscard]] BoundNodeKind Kind() const override {
        return BoundNodeKind::FunctionExpression;
    }
    [[nodiscard]] const semantic::FunctionSymbol& Symbol() const {
        return _symbol;
    }
    [[nodiscard]] const std::vector<BoundClosureCapture>& Captures() const {
        return _captures;
    }

   private:
    const semantic::FunctionSymbol& _symbol;
    std::vector<BoundClosureCapture> _captures;
};

class BoundUnaryExpression final : public BoundExpression {
   public:
    BoundUnaryExpression(source::SourceRange range, semantic::SemanticType type,
                         TokenType operatorType,
                         std::unique_ptr<BoundExpression> operand)
        : BoundExpression(range, type),
          _operatorType(operatorType),
          _operand(std::move(operand)) {}
    [[nodiscard]] BoundNodeKind Kind() const override {
        return BoundNodeKind::UnaryExpression;
    }
    [[nodiscard]] TokenType OperatorType() const { return _operatorType; }
    [[nodiscard]] const BoundExpression& Operand() const { return *_operand; }

   private:
    TokenType _operatorType;
    std::unique_ptr<BoundExpression> _operand;
};

class BoundCastExpression final : public BoundExpression {
   public:
    BoundCastExpression(source::SourceRange range,
                        semantic::SemanticType targetType,
                        std::unique_ptr<BoundExpression> expression)
        : BoundExpression(range, targetType),
          _expression(std::move(expression)) {}
    [[nodiscard]] BoundNodeKind Kind() const override {
        return BoundNodeKind::CastExpression;
    }
    [[nodiscard]] const BoundExpression& Value() const { return *_expression; }

   private:
    std::unique_ptr<BoundExpression> _expression;
};

class BoundBinaryExpression final : public BoundExpression {
   public:
    BoundBinaryExpression(source::SourceRange range,
                          semantic::SemanticType type,
                          std::unique_ptr<BoundExpression> left,
                          TokenType operatorType,
                          std::unique_ptr<BoundExpression> right)
        : BoundExpression(range, type),
          _left(std::move(left)),
          _operatorType(operatorType),
          _right(std::move(right)) {}
    [[nodiscard]] BoundNodeKind Kind() const override {
        return BoundNodeKind::BinaryExpression;
    }
    [[nodiscard]] const BoundExpression& Left() const { return *_left; }
    [[nodiscard]] TokenType OperatorType() const { return _operatorType; }
    [[nodiscard]] const BoundExpression& Right() const { return *_right; }

   private:
    std::unique_ptr<BoundExpression> _left;
    TokenType _operatorType;
    std::unique_ptr<BoundExpression> _right;
};

enum class OverflowingArithmeticOperation {
    Add,
    Subtract,
    Multiply,
};

class BoundOverflowingArithmeticExpression final : public BoundExpression {
   public:
    BoundOverflowingArithmeticExpression(
        source::SourceRange range, semantic::SemanticType type,
        OverflowingArithmeticOperation operation,
        std::unique_ptr<BoundExpression> left,
        std::unique_ptr<BoundExpression> right)
        : BoundExpression(range, type),
          _operation(operation),
          _left(std::move(left)),
          _right(std::move(right)) {}
    [[nodiscard]] BoundNodeKind Kind() const override {
        return BoundNodeKind::OverflowingArithmeticExpression;
    }
    [[nodiscard]] OverflowingArithmeticOperation Operation() const {
        return _operation;
    }
    [[nodiscard]] const BoundExpression& Left() const { return *_left; }
    [[nodiscard]] const BoundExpression& Right() const { return *_right; }

   private:
    OverflowingArithmeticOperation _operation;
    std::unique_ptr<BoundExpression> _left;
    std::unique_ptr<BoundExpression> _right;
};

class BoundIntegerLimitExpression final : public BoundExpression {
   public:
    BoundIntegerLimitExpression(source::SourceRange range,
                                semantic::SemanticType type, bool maximum)
        : BoundExpression(range, type), _maximum(maximum) {}
    [[nodiscard]] BoundNodeKind Kind() const override {
        return BoundNodeKind::IntegerLimitExpression;
    }
    [[nodiscard]] bool IsMaximum() const { return _maximum; }

   private:
    bool _maximum;
};

enum class FloatingConstant {
    NaN,
    Infinity,
    NegativeInfinity,
    Minimum,
    Maximum,
    MinimumPositive,
    Epsilon,
};

class BoundFloatingConstantExpression final : public BoundExpression {
   public:
    BoundFloatingConstantExpression(source::SourceRange range,
                                    semantic::SemanticType type,
                                    FloatingConstant constant)
        : BoundExpression(range, type), _constant(constant) {}
    [[nodiscard]] BoundNodeKind Kind() const override {
        return BoundNodeKind::FloatingConstantExpression;
    }
    [[nodiscard]] FloatingConstant Constant() const { return _constant; }

   private:
    FloatingConstant _constant;
};

class BoundIncrementExpression final : public BoundExpression {
   public:
    BoundIncrementExpression(source::SourceRange range,
                             std::unique_ptr<BoundPlaceExpression> target,
                             TokenType operatorType)
        : BoundExpression(range, target->Type().WithoutConst()),
          _target(std::move(target)),
          _operatorType(operatorType) {}
    [[nodiscard]] BoundNodeKind Kind() const override {
        return BoundNodeKind::IncrementExpression;
    }
    [[nodiscard]] const BoundPlaceExpression& Target() const {
        return *_target;
    }
    [[nodiscard]] TokenType OperatorType() const { return _operatorType; }

   private:
    std::unique_ptr<BoundPlaceExpression> _target;
    TokenType _operatorType;
};

class BoundAssignmentExpression final : public BoundExpression {
   public:
    BoundAssignmentExpression(source::SourceRange range,
                              std::unique_ptr<BoundPlaceExpression> target,
                              std::unique_ptr<BoundExpression> value,
                              TokenType operatorType = TokenType::Assign)
        : BoundExpression(range, target->Type()),
          _target(std::move(target)),
          _value(std::move(value)),
          _operatorType(operatorType) {}
    [[nodiscard]] BoundNodeKind Kind() const override {
        return BoundNodeKind::AssignmentExpression;
    }
    [[nodiscard]] const BoundPlaceExpression& Target() const {
        return *_target;
    }
    [[nodiscard]] const BoundExpression& Value() const { return *_value; }
    [[nodiscard]] TokenType OperatorType() const { return _operatorType; }

   private:
    std::unique_ptr<BoundPlaceExpression> _target;
    std::unique_ptr<BoundExpression> _value;
    TokenType _operatorType;
};

class BoundAllocationExpression final : public BoundExpression {
   public:
    BoundAllocationExpression(source::SourceRange range,
                              semantic::SemanticType result,
                              semantic::SemanticType element,
                              semantic::SemanticType failure,
                              std::unique_ptr<BoundExpression> initializer,
                              bool dynamic = false, bool uninitialized = false)
        : BoundExpression(range, result),
          _element(element),
          _failure(failure),
          _initializer(std::move(initializer)),
          _dynamic(dynamic),
          _uninitialized(uninitialized) {}
    BoundNodeKind Kind() const override {
        return BoundNodeKind::AllocationExpression;
    }
    bool IsDynamic() const { return _dynamic; }
    bool IsUninitialized() const { return _uninitialized; }
    semantic::SemanticType ElementType() const { return _element; }
    semantic::SemanticType FailureType() const { return _failure; }
    const BoundExpression& Initializer() const { return *_initializer; }

   private:
    semantic::SemanticType _element;
    semantic::SemanticType _failure;
    std::unique_ptr<BoundExpression> _initializer;
    bool _dynamic;
    bool _uninitialized;
};

class BoundCallExpression final : public BoundExpression {
   public:
    BoundCallExpression(source::SourceRange range,
                        const semantic::FunctionSymbol& function,
                        std::vector<std::unique_ptr<BoundExpression>> arguments)
        : BoundExpression(range, function.ReturnType().WithoutConst()),
          _function(function),
          _arguments(std::move(arguments)) {}
    [[nodiscard]] BoundNodeKind Kind() const override {
        return BoundNodeKind::CallExpression;
    }
    [[nodiscard]] const semantic::FunctionSymbol& Function() const {
        return _function;
    }
    [[nodiscard]] const std::vector<std::unique_ptr<BoundExpression>>&
    Arguments() const {
        return _arguments;
    }

   private:
    const semantic::FunctionSymbol& _function;
    std::vector<std::unique_ptr<BoundExpression>> _arguments;
};

class BoundIndirectCallExpression final : public BoundExpression {
   public:
    BoundIndirectCallExpression(
        source::SourceRange range, std::unique_ptr<BoundExpression> callee,
        std::vector<std::unique_ptr<BoundExpression>> arguments)
        : BoundExpression(range,
                          callee->Type().Signature().result.WithoutConst()),
          _callee(std::move(callee)),
          _arguments(std::move(arguments)) {}
    [[nodiscard]] BoundNodeKind Kind() const override {
        return BoundNodeKind::IndirectCallExpression;
    }
    [[nodiscard]] const BoundExpression& Callee() const { return *_callee; }
    [[nodiscard]] const std::vector<std::unique_ptr<BoundExpression>>&
    Arguments() const {
        return _arguments;
    }

   private:
    std::unique_ptr<BoundExpression> _callee;
    std::vector<std::unique_ptr<BoundExpression>> _arguments;
};

class BoundStringViewOperationExpression final : public BoundExpression {
   public:
    BoundStringViewOperationExpression(
        source::SourceRange range, semantic::SemanticType type,
        StringViewOperation operation,
        std::unique_ptr<BoundExpression> receiver,
        std::vector<std::unique_ptr<BoundExpression>> arguments)
        : BoundExpression(range, type),
          _operation(operation),
          _receiver(std::move(receiver)),
          _arguments(std::move(arguments)) {}

    [[nodiscard]] BoundNodeKind Kind() const override {
        return BoundNodeKind::StringViewOperationExpression;
    }
    [[nodiscard]] StringViewOperation Operation() const { return _operation; }
    [[nodiscard]] const BoundExpression& Receiver() const { return *_receiver; }
    [[nodiscard]] const std::vector<std::unique_ptr<BoundExpression>>&
    Arguments() const {
        return _arguments;
    }

   private:
    StringViewOperation _operation;
    std::unique_ptr<BoundExpression> _receiver;
    std::vector<std::unique_ptr<BoundExpression>> _arguments;
};

class BoundBlockStatement final : public BoundStatement {
   public:
    BoundBlockStatement(source::SourceRange range,
                        std::vector<std::unique_ptr<BoundStatement>> statements)
        : BoundStatement(range), _statements(std::move(statements)) {}
    [[nodiscard]] BoundNodeKind Kind() const override {
        return BoundNodeKind::BlockStatement;
    }
    [[nodiscard]] const std::vector<std::unique_ptr<BoundStatement>>&
    Statements() const {
        return _statements;
    }

   private:
    std::vector<std::unique_ptr<BoundStatement>> _statements;
};

class BoundVariableDeclarationStatement final : public BoundStatement {
   public:
    BoundVariableDeclarationStatement(
        source::SourceRange range, const semantic::VariableSymbol& variable,
        std::unique_ptr<BoundExpression> initializer)
        : BoundStatement(range),
          _variable(variable),
          _initializer(std::move(initializer)) {}
    [[nodiscard]] BoundNodeKind Kind() const override {
        return BoundNodeKind::VariableDeclarationStatement;
    }
    [[nodiscard]] const semantic::VariableSymbol& Variable() const {
        return _variable;
    }
    [[nodiscard]] const BoundExpression* Initializer() const {
        return _initializer.get();
    }

   private:
    const semantic::VariableSymbol& _variable;
    std::unique_ptr<BoundExpression> _initializer;
};

class BoundDeferStatement final : public BoundStatement {
   public:
    BoundDeferStatement(source::SourceRange range,
                        std::unique_ptr<BoundExpression> call)
        : BoundStatement(range), _call(std::move(call)) {}
    [[nodiscard]] BoundNodeKind Kind() const override {
        return BoundNodeKind::DeferStatement;
    }
    [[nodiscard]] const BoundExpression& Call() const { return *_call; }

   private:
    std::unique_ptr<BoundExpression> _call;
};

class BoundExpressionStatement final : public BoundStatement {
   public:
    BoundExpressionStatement(source::SourceRange range,
                             std::unique_ptr<BoundExpression> expression)
        : BoundStatement(range), _expression(std::move(expression)) {}
    [[nodiscard]] BoundNodeKind Kind() const override {
        return BoundNodeKind::ExpressionStatement;
    }
    [[nodiscard]] const BoundExpression& Expression() const {
        return *_expression;
    }

   private:
    std::unique_ptr<BoundExpression> _expression;
};

class BoundReturnStatement final : public BoundStatement {
   public:
    BoundReturnStatement(source::SourceRange range,
                         std::unique_ptr<BoundExpression> expression)
        : BoundStatement(range), _expression(std::move(expression)) {}
    [[nodiscard]] BoundNodeKind Kind() const override {
        return BoundNodeKind::ReturnStatement;
    }
    [[nodiscard]] const BoundExpression* Expression() const {
        return _expression.get();
    }

   private:
    std::unique_ptr<BoundExpression> _expression;
};

class BoundIfStatement final : public BoundStatement {
   public:
    BoundIfStatement(source::SourceRange range,
                     std::unique_ptr<BoundExpression> condition,
                     std::unique_ptr<BoundBlockStatement> thenBlock,
                     std::unique_ptr<BoundBlockStatement> elseBlock)
        : BoundStatement(range),
          _condition(std::move(condition)),
          _thenBlock(std::move(thenBlock)),
          _elseBlock(std::move(elseBlock)) {}
    [[nodiscard]] BoundNodeKind Kind() const override {
        return BoundNodeKind::IfStatement;
    }
    [[nodiscard]] const BoundExpression& Condition() const {
        return *_condition;
    }
    [[nodiscard]] const BoundBlockStatement& ThenBlock() const {
        return *_thenBlock;
    }
    [[nodiscard]] const BoundBlockStatement* ElseBlock() const {
        return _elseBlock.get();
    }

   private:
    std::unique_ptr<BoundExpression> _condition;
    std::unique_ptr<BoundBlockStatement> _thenBlock;
    std::unique_ptr<BoundBlockStatement> _elseBlock;
};

class BoundWhileStatement final : public BoundStatement {
   public:
    BoundWhileStatement(source::SourceRange range,
                        std::unique_ptr<BoundExpression> condition,
                        std::unique_ptr<BoundBlockStatement> body,
                        std::unique_ptr<BoundExpression> update = nullptr)
        : BoundStatement(range),
          _condition(std::move(condition)),
          _body(std::move(body)),
          _update(std::move(update)) {}
    [[nodiscard]] BoundNodeKind Kind() const override {
        return BoundNodeKind::WhileStatement;
    }
    [[nodiscard]] const BoundExpression& Condition() const {
        return *_condition;
    }
    [[nodiscard]] const BoundBlockStatement& Body() const { return *_body; }
    [[nodiscard]] const BoundExpression* Update() const {
        return _update.get();
    }

   private:
    std::unique_ptr<BoundExpression> _condition;
    std::unique_ptr<BoundBlockStatement> _body;
    std::unique_ptr<BoundExpression> _update;
};

class BoundProceedStatement final : public BoundStatement {
   public:
    using BoundStatement::BoundStatement;
    [[nodiscard]] BoundNodeKind Kind() const override {
        return BoundNodeKind::ProceedStatement;
    }
};

class BoundBreakStatement final : public BoundStatement {
   public:
    using BoundStatement::BoundStatement;
    [[nodiscard]] BoundNodeKind Kind() const override {
        return BoundNodeKind::BreakStatement;
    }
};

class BoundJumpToStatement final : public BoundStatement {
   public:
    BoundJumpToStatement(source::SourceRange range, std::string position)
        : BoundStatement(range), _position(std::move(position)) {}
    [[nodiscard]] BoundNodeKind Kind() const override {
        return BoundNodeKind::JumpToStatement;
    }
    [[nodiscard]] const std::string& Position() const { return _position; }

   private:
    std::string _position;
};

class BoundPositionStatement final : public BoundStatement {
   public:
    BoundPositionStatement(source::SourceRange range, std::string name)
        : BoundStatement(range), _name(std::move(name)) {}
    [[nodiscard]] BoundNodeKind Kind() const override {
        return BoundNodeKind::PositionStatement;
    }
    [[nodiscard]] const std::string& Name() const { return _name; }

   private:
    std::string _name;
};

class BoundBuildExpression final : public BoundExpression {
   public:
    BoundBuildExpression(source::SourceRange range, semantic::SemanticType type,
                         const semantic::VariableSymbol& self,
                         std::vector<std::unique_ptr<BoundExpression>> defaults,
                         std::unique_ptr<BoundBlockStatement> body)
        : BoundExpression(range, type),
          _self(self),
          _defaults(std::move(defaults)),
          _body(std::move(body)) {}
    BoundNodeKind Kind() const override {
        return BoundNodeKind::BuildExpression;
    }
    const semantic::VariableSymbol& Self() const { return _self; }
    const auto& Defaults() const { return _defaults; }
    const BoundBlockStatement& Body() const { return *_body; }

   private:
    const semantic::VariableSymbol& _self;
    std::vector<std::unique_ptr<BoundExpression>> _defaults;
    std::unique_ptr<BoundBlockStatement> _body;
};

class BoundFunction final {
   public:
    BoundFunction(const semantic::FunctionSymbol& symbol,
                  std::vector<BoundFunctionCapture> captures,
                  std::vector<const semantic::VariableSymbol*> parameters,
                  std::unique_ptr<BoundBlockStatement> body)
        : _symbol(symbol),
          _captures(std::move(captures)),
          _parameters(std::move(parameters)),
          _body(std::move(body)) {}
    [[nodiscard]] const semantic::FunctionSymbol& Symbol() const {
        return _symbol;
    }
    [[nodiscard]] const std::vector<BoundFunctionCapture>& Captures() const {
        return _captures;
    }
    [[nodiscard]] const std::vector<const semantic::VariableSymbol*>&
    Parameters() const {
        return _parameters;
    }
    [[nodiscard]] const BoundBlockStatement& Body() const { return *_body; }
    [[nodiscard]] bool IsExtern() const { return _symbol.IsExtern(); }

   private:
    const semantic::FunctionSymbol& _symbol;
    std::vector<BoundFunctionCapture> _captures;
    std::vector<const semantic::VariableSymbol*> _parameters;
    std::unique_ptr<BoundBlockStatement> _body;
};

class BoundProgram final {
   public:
    explicit BoundProgram(std::vector<std::unique_ptr<BoundFunction>> functions)
        : _functions(std::move(functions)) {}
    BoundProgram(const BoundProgram&) = delete;
    BoundProgram& operator=(const BoundProgram&) = delete;
    BoundProgram(BoundProgram&&) = default;
    BoundProgram& operator=(BoundProgram&&) = default;
    [[nodiscard]] const std::vector<std::unique_ptr<BoundFunction>>& Functions()
        const {
        return _functions;
    }

   private:
    std::vector<std::unique_ptr<BoundFunction>> _functions;
};

}  // namespace flecha::lowering

#endif  // FLECHA_LOWERING_BOUND_IR_HPP
