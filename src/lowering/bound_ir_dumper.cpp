#include "lowering/bound_ir_dumper.hpp"

#include <sstream>
#include <string>

namespace flecha::lowering {
namespace {

std::string OperatorText(TokenType type) {
    switch (type) {
        case TokenType::Assign:
            return "=";
        case TokenType::Add:
            return "+";
        case TokenType::AddAssign:
            return "+=";
        case TokenType::Sub:
            return "-";
        case TokenType::SubAssign:
            return "-=";
        case TokenType::Mul:
            return "*";
        case TokenType::MulAssign:
            return "*=";
        case TokenType::Div:
            return "/";
        case TokenType::DivAssign:
            return "/=";
        case TokenType::Mod:
            return "%";
        case TokenType::Xor:
            return "^";
        case TokenType::XorAssign:
            return "^=";
        case TokenType::BitOr:
            return "|";
        case TokenType::BitNot:
            return "~";
        case TokenType::ShiftLeft:
            return "<<";
        case TokenType::ShiftRight:
            return ">>";
        case TokenType::Bang:
            return "!";
        case TokenType::Equal:
            return "==";
        case TokenType::NotEqual:
            return "!=";
        case TokenType::StrictEqual:
            return "===";
        case TokenType::StrictNotEqual:
            return "!==";
        case TokenType::Ampersand:
            return "&";
        case TokenType::And:
            return "and";
        case TokenType::Or:
            return "or";
        case TokenType::Less:
            return "<";
        case TokenType::LessEqual:
            return "<=";
        case TokenType::Greater:
            return ">";
        case TokenType::GreaterEqual:
            return ">=";
        default:
            return "?";
    }
}

std::string FloatingConstantText(FloatingConstant constant) {
    switch (constant) {
        case FloatingConstant::NaN:
            return "nan";
        case FloatingConstant::Infinity:
            return "infinity";
        case FloatingConstant::NegativeInfinity:
            return "negative_infinity";
        case FloatingConstant::Minimum:
            return "min_value";
        case FloatingConstant::Maximum:
            return "max_value";
        case FloatingConstant::MinimumPositive:
            return "min_positive";
        case FloatingConstant::Epsilon:
            return "epsilon";
    }
    return "?";
}

class Dumper final {
   public:
    std::string Dump(const BoundProgram& program) {
        for (const std::unique_ptr<BoundFunction>& function :
             program.Functions()) {
            WriteFunction(*function);
        }
        return _stream.str();
    }

   private:
    void Indent() {
        for (int index = 0; index < _indent; ++index) _stream << "  ";
    }

    void WriteFunction(const BoundFunction& function) {
        if (function.Symbol().IsExtern()) _stream << "extern ";
        if (function.Symbol().IsUnsafe()) _stream << "unsafe ";
        _stream << "function " << function.Symbol().Name() << "(";
        for (std::size_t index = 0; index < function.Parameters().size();
             ++index) {
            if (index > 0) _stream << ", ";
            const semantic::VariableSymbol& parameter =
                *function.Parameters()[index];
            _stream << parameter.Type().Name() << " " << parameter.Name();
        }
        _stream << ") -> " << function.Symbol().ReturnType().Name() << "\n";
        WriteBlock(function.Body());
    }

    void WriteBlock(const BoundBlockStatement& block) {
        Indent();
        _stream << "{\n";
        ++_indent;
        for (const std::unique_ptr<BoundStatement>& statement :
             block.Statements()) {
            WriteStatement(*statement);
        }
        --_indent;
        Indent();
        _stream << "}\n";
    }

    void WriteStatement(const BoundStatement& statement) {
        Indent();
        switch (statement.Kind()) {
            case BoundNodeKind::BlockStatement:
                WriteBlock(static_cast<const BoundBlockStatement&>(statement));
                break;
            case BoundNodeKind::VariableDeclarationStatement: {
                const BoundVariableDeclarationStatement& declaration =
                    static_cast<const BoundVariableDeclarationStatement&>(
                        statement);
                _stream << "local " << declaration.Variable().Name() << ": "
                        << declaration.Variable().Type().Name();
                if (declaration.Initializer() != nullptr) {
                    _stream << " = ";
                    WriteExpression(*declaration.Initializer());
                }
                _stream << "\n";
                break;
            }
            case BoundNodeKind::DestructureStatement: {
                const auto& destructure =
                    static_cast<const BoundDestructureStatement&>(statement);
                _stream << "destructure (";
                bool first = true;
                for (const auto& binding : destructure.Bindings()) {
                    if (!first) _stream << ", ";
                    first = false;
                    if (binding.variable == nullptr)
                        _stream << "_";
                    else
                        _stream << (binding.declaration ? "local " : "")
                                << binding.variable->Name() << ": "
                                << binding.variable->Type().Name();
                }
                _stream << ") = ";
                WriteExpression(destructure.Value());
                _stream << "\n";
                break;
            }
            case BoundNodeKind::DeferStatement:
                _stream << "defer ";
                WriteExpression(
                    static_cast<const BoundDeferStatement&>(statement).Call());
                _stream << "\n";
                break;
            case BoundNodeKind::ExpressionStatement:
                WriteExpression(
                    static_cast<const BoundExpressionStatement&>(statement)
                        .Expression());
                _stream << "\n";
                break;
            case BoundNodeKind::ReturnStatement: {
                const BoundReturnStatement& returnStatement =
                    static_cast<const BoundReturnStatement&>(statement);
                _stream << "return";
                if (returnStatement.Expression() != nullptr) {
                    _stream << " ";
                    WriteExpression(*returnStatement.Expression());
                }
                _stream << "\n";
                break;
            }
            case BoundNodeKind::IfStatement: {
                const BoundIfStatement& ifStatement =
                    static_cast<const BoundIfStatement&>(statement);
                _stream << "if ";
                WriteExpression(ifStatement.Condition());
                _stream << "\n";
                WriteBlock(ifStatement.ThenBlock());
                if (ifStatement.ElseBlock() != nullptr) {
                    Indent();
                    _stream << "else\n";
                    WriteBlock(*ifStatement.ElseBlock());
                }
                break;
            }
            case BoundNodeKind::WhileStatement: {
                const BoundWhileStatement& whileStatement =
                    static_cast<const BoundWhileStatement&>(statement);
                _stream << "while ";
                WriteExpression(whileStatement.Condition());
                _stream << "\n";
                WriteBlock(whileStatement.Body());
                if (whileStatement.Update() != nullptr) {
                    Indent();
                    _stream << "update ";
                    WriteExpression(*whileStatement.Update());
                    _stream << "\n";
                }
                break;
            }
            case BoundNodeKind::ProceedStatement:
                _stream << "proceed\n";
                break;
            case BoundNodeKind::BreakStatement:
                _stream << "break\n";
                break;
            case BoundNodeKind::JumpToStatement:
                _stream << "jumpto "
                        << static_cast<const BoundJumpToStatement&>(statement)
                               .Position()
                        << "\n";
                break;
            case BoundNodeKind::PositionStatement:
                _stream << "position "
                        << static_cast<const BoundPositionStatement&>(statement)
                               .Name()
                        << "\n";
                break;
            default:
                break;
        }
    }

    void WriteExpression(const BoundExpression& expression) {
        switch (expression.Kind()) {
            case BoundNodeKind::DefaultExpression:
                _stream << "default " << expression.Type().Name();
                break;
            case BoundNodeKind::FieldExpression: {
                const auto& field =
                    static_cast<const BoundFieldExpression&>(expression);
                WriteExpression(field.Receiver());
                _stream << (field.Indirect() ? "->" : ".") << field.Index();
                break;
            }
            case BoundNodeKind::BuildExpression: {
                const auto& build =
                    static_cast<const BoundBuildExpression&>(expression);
                _stream << "build " << expression.Type().Name() << " ";
                WriteBlock(build.Body());
                break;
            }

            case BoundNodeKind::SliceExpression: {
                const auto& slice =
                    static_cast<const BoundSliceExpression&>(expression);
                _stream << "(slice " << slice.Type().Name() << " ";
                WriteExpression(slice.Receiver());
                _stream << " ";
                if (slice.Start() != nullptr)
                    WriteExpression(*slice.Start());
                else
                    _stream << "0";
                _stream << ":";
                if (slice.End() != nullptr)
                    WriteExpression(*slice.End());
                else
                    _stream << "len";
                _stream << ")";
                break;
            }
            case BoundNodeKind::TupleExpression: {
                const auto& tuple =
                    static_cast<const BoundTupleExpression&>(expression);
                _stream << "(";
                bool first = true;
                for (const auto& element : tuple.Elements()) {
                    if (!first) _stream << ", ";
                    first = false;
                    WriteExpression(*element);
                }
                _stream << ")";
                break;
            }
            case BoundNodeKind::ArrayExpression: {
                const auto& array =
                    static_cast<const BoundArrayExpression&>(expression);
                _stream << (array.IsRepeat() ? "(repeat " : "(array ")
                        << array.Type().Name();
                for (const auto& element : array.Elements()) {
                    _stream << " ";
                    WriteExpression(*element);
                }
                _stream << ")";
                break;
            }
            case BoundNodeKind::MatchExpression: {
                const auto& match =
                    static_cast<const BoundMatchExpression&>(expression);
                _stream << "(match ";
                WriteExpression(match.Matched());
                for (const auto& arm : match.Arms()) {
                    _stream << " (";
                    if (arm.discriminant.has_value())
                        _stream << *arm.discriminant;
                    else
                        _stream << "_";
                    _stream << " => ";
                    WriteExpression(*arm.value);
                    _stream << ")";
                }
                _stream << ")";
                break;
            }
            case BoundNodeKind::ArrayElementExpression: {
                const auto& index =
                    static_cast<const BoundArrayElementExpression&>(expression);
                _stream << "(checked-index ";
                WriteExpression(index.Receiver());
                _stream << " ";
                if (index.HasIndex())
                    WriteExpression(index.Index());
                else
                    _stream << (index.FromBack() ? "back" : "front");
                _stream << ")";
                break;
            }
            case BoundNodeKind::ArrayLengthExpression:
                _stream << "(array-len ";
                WriteExpression(
                    static_cast<const BoundArrayLengthExpression&>(expression)
                        .Receiver());
                _stream << ")";
                break;
            case BoundNodeKind::SliceDataExpression:
                _stream << "(slice-data ";
                WriteExpression(
                    static_cast<const BoundSliceDataExpression&>(expression)
                        .Receiver());
                _stream << ")";
                break;
            case BoundNodeKind::DereferenceExpression:
                _stream << "(deref ";
                WriteExpression(
                    static_cast<const BoundDereferenceExpression&>(expression)
                        .Pointer());
                _stream << ")";
                break;
            case BoundNodeKind::AddressExpression:
                _stream << "(address ";
                WriteExpression(
                    static_cast<const BoundAddressExpression&>(expression)
                        .Place());
                _stream << ")";
                break;
            case BoundNodeKind::LiteralExpression:
                _stream << static_cast<const BoundLiteralExpression&>(
                               expression)
                               .Value()
                        << ":" << expression.Type().Name();
                break;
            case BoundNodeKind::VariableExpression:
                _stream << "load "
                        << static_cast<const BoundVariableExpression&>(
                               expression)
                               .Symbol()
                               .Name();
                break;
            case BoundNodeKind::MoveExpression:
                _stream << "move ";
                WriteExpression(
                    static_cast<const BoundMoveExpression&>(expression)
                        .Source());
                break;
            case BoundNodeKind::DirectExpression:
                _stream << "direct ";
                WriteExpression(
                    static_cast<const BoundDirectExpression&>(expression)
                        .Value());
                break;
            case BoundNodeKind::FunctionExpression: {
                const auto& function =
                    static_cast<const BoundFunctionExpression&>(expression);
                _stream << "function_ref " << function.Symbol().Name();
                if (!function.Captures().empty()) {
                    _stream << " [";
                    bool first = true;
                    for (const auto& capture : function.Captures()) {
                        if (!first) _stream << ", ";
                        first = false;
                        switch (capture.mode) {
                            case core::AnonymousFunctionExpression::
                                CaptureMode::Copy:
                                _stream << "copy ";
                                break;
                            case core::AnonymousFunctionExpression::
                                CaptureMode::Reference:
                                _stream << "&";
                                break;
                            case core::AnonymousFunctionExpression::
                                CaptureMode::ConstReference:
                                _stream << "&const ";
                                break;
                            case core::AnonymousFunctionExpression::
                                CaptureMode::Lend:
                                _stream << "lend ";
                                break;
                            case core::AnonymousFunctionExpression::
                                CaptureMode::Own:
                                _stream << "own ";
                                break;
                        }
                        WriteExpression(*capture.value);
                    }
                    _stream << "]";
                }
            } break;
            case BoundNodeKind::IncrementExpression: {
                const auto& update =
                    static_cast<const BoundIncrementExpression&>(expression);
                _stream << (update.OperatorType() == TokenType::Increment
                                ? "(post-increment "
                                : "(post-decrement ");
                WriteExpression(update.Target());
                _stream << ")";
                break;
            }
            case BoundNodeKind::UnaryExpression: {
                const BoundUnaryExpression& unary =
                    static_cast<const BoundUnaryExpression&>(expression);
                _stream << "(" << OperatorText(unary.OperatorType()) << " ";
                WriteExpression(unary.Operand());
                _stream << ")";
                break;
            }
            case BoundNodeKind::CastExpression: {
                const BoundCastExpression& cast =
                    static_cast<const BoundCastExpression&>(expression);
                _stream << "(cast " << cast.Type().Name() << " ";
                WriteExpression(cast.Value());
                _stream << ")";
                break;
            }
            case BoundNodeKind::BinaryExpression: {
                const BoundBinaryExpression& binary =
                    static_cast<const BoundBinaryExpression&>(expression);
                _stream << "(" << OperatorText(binary.OperatorType()) << " ";
                WriteExpression(binary.Left());
                _stream << " ";
                WriteExpression(binary.Right());
                _stream << ")";
                break;
            }
            case BoundNodeKind::IntegerLimitExpression: {
                const auto& limit =
                    static_cast<const BoundIntegerLimitExpression&>(expression);
                _stream << "(" << limit.Type().Name() << "."
                        << (limit.IsMaximum() ? "max_value" : "min_value")
                        << ")";
                break;
            }
            case BoundNodeKind::FloatingConstantExpression: {
                const auto& constant =
                    static_cast<const BoundFloatingConstantExpression&>(
                        expression);
                _stream << "(" << constant.Type().Name() << "."
                        << FloatingConstantText(constant.Constant()) << ")";
                break;
            }
            case BoundNodeKind::OverflowingArithmeticExpression: {
                const auto& arithmetic =
                    static_cast<const BoundOverflowingArithmeticExpression&>(
                        expression);
                _stream << "(overflowing_";
                switch (arithmetic.Operation()) {
                    case OverflowingArithmeticOperation::Add:
                        _stream << "add";
                        break;
                    case OverflowingArithmeticOperation::Subtract:
                        _stream << "sub";
                        break;
                    case OverflowingArithmeticOperation::Multiply:
                        _stream << "mul";
                        break;
                }
                _stream << " ";
                WriteExpression(arithmetic.Left());
                _stream << " ";
                WriteExpression(arithmetic.Right());
                _stream << ")";
                break;
            }
            case BoundNodeKind::AssignmentExpression: {
                const BoundAssignmentExpression& assignment =
                    static_cast<const BoundAssignmentExpression&>(expression);
                if (assignment.OperatorType() == TokenType::Assign)
                    _stream << "(store ";
                else
                    _stream << "(" << OperatorText(assignment.OperatorType())
                            << "-store ";
                if (assignment.Target().Kind() ==
                    BoundNodeKind::VariableExpression) {
                    _stream << static_cast<const BoundVariableExpression&>(
                                   assignment.Target())
                                   .Symbol()
                                   .Name();
                } else {
                    WriteExpression(assignment.Target());
                }
                _stream << " ";
                WriteExpression(assignment.Value());
                _stream << ")";
                break;
            }
            case BoundNodeKind::AllocationExpression: {
                const auto& allocation =
                    static_cast<const BoundAllocationExpression&>(expression);
                _stream << "(new " << allocation.ElementType().Name() << " ";
                WriteExpression(allocation.Initializer());
                _stream << ")";
                break;
            }
            case BoundNodeKind::CallExpression: {
                const BoundCallExpression& call =
                    static_cast<const BoundCallExpression&>(expression);
                _stream << "(call " << call.Function().Name();
                for (const std::unique_ptr<BoundExpression>& argument :
                     call.Arguments()) {
                    _stream << " ";
                    WriteExpression(*argument);
                }
                _stream << ")";
                break;
            }
            case BoundNodeKind::IndirectCallExpression: {
                const auto& call =
                    static_cast<const BoundIndirectCallExpression&>(expression);
                _stream << "(call_indirect ";
                WriteExpression(call.Callee());
                for (const auto& argument : call.Arguments()) {
                    _stream << " ";
                    WriteExpression(*argument);
                }
                _stream << ")";
                break;
            }
            case BoundNodeKind::StringViewOperationExpression: {
                const BoundStringViewOperationExpression& operation =
                    static_cast<const BoundStringViewOperationExpression&>(
                        expression);
                const char* name = "len";
                if (operation.Operation() == StringViewOperation::At) {
                    name = "at";
                } else if (operation.Operation() ==
                           StringViewOperation::Substring) {
                    name = "substr";
                } else if (operation.Operation() ==
                           StringViewOperation::ToString) {
                    name = "string";
                } else if (operation.Operation() == StringViewOperation::View) {
                    name = "view";
                } else if (operation.Operation() == StringViewOperation::Copy) {
                    name = "copy";
                } else if (operation.Operation() == StringViewOperation::Data) {
                    name = "data";
                }
                _stream << "(string." << name << " ";
                WriteExpression(operation.Receiver());
                for (const std::unique_ptr<BoundExpression>& argument :
                     operation.Arguments()) {
                    _stream << " ";
                    WriteExpression(*argument);
                }
                _stream << ")";
                break;
            }
            default:
                break;
        }
    }

    std::ostringstream _stream;
    int _indent = 0;
};

}  // namespace

std::string DumpBoundProgram(const BoundProgram& program) {
    Dumper dumper;
    return dumper.Dump(program);
}

}  // namespace flecha::lowering
