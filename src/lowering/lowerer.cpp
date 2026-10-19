#include "lowering/lowerer.hpp"

#include <memory>
#include <utility>
#include <vector>

namespace flecha::lowering {

std::unique_ptr<BoundProgram> Lowerer::Lower(
    const core::CompilationUnit& unit,
    const semantic::SemanticModel& semanticModel,
    const diagnostics::DiagnosticBag& diagnostics) {
    if (diagnostics.HasErrors()) {
        return nullptr;
    }

    _semanticModel = &semanticModel;
    std::vector<std::unique_ptr<BoundFunction>> functions;
    functions.reserve(unit.Declarations().size());
    for (const std::unique_ptr<core::Declaration>& declaration :
         unit.Declarations()) {
        if (declaration->Kind() != core::AstNodeKind::FunctionDeclaration) {
            continue;
        }
        functions.push_back(LowerFunction(
            static_cast<const core::FunctionDeclaration&>(*declaration)));
    }
    for (const auto& function : semanticModel.GeneratedFunctions())
        functions.push_back(LowerFunction(*function));
    for (const auto* function : semanticModel.AnonymousFunctions())
        functions.push_back(LowerFunction(*function));
    _semanticModel = nullptr;
    return std::make_unique<BoundProgram>(std::move(functions));
}

std::unique_ptr<BoundFunction> Lowerer::LowerFunction(
    const core::FunctionDeclaration& function) {
    const semantic::Symbol* symbol = _semanticModel->DeclaredSymbol(function);
    const semantic::FunctionSymbol& functionSymbol =
        static_cast<const semantic::FunctionSymbol&>(*symbol);

    std::vector<const semantic::VariableSymbol*> parameters;
    parameters.reserve(function.Parameters().size());
    for (const std::unique_ptr<core::Parameter>& parameter :
         function.Parameters()) {
        const semantic::Symbol* parameterSymbol =
            _semanticModel->DeclaredSymbol(*parameter);
        parameters.push_back(
            static_cast<const semantic::VariableSymbol*>(parameterSymbol));
    }

    std::vector<BoundFunctionCapture> captures;
    for (const semantic::ClosureCapture& capture :
         _semanticModel->Captures(function))
        captures.push_back({capture.syntax->Mode(), capture.variable});

    _returnType = functionSymbol.ReturnType();
    _zeroFunction =
        functionSymbol.Owner() && functionSymbol.Name().ends_with("::zero");
    auto result = std::make_unique<BoundFunction>(
        functionSymbol, std::move(captures), std::move(parameters),
        LowerBlock(function.Body()));
    _zeroFunction = false;
    return result;
}

std::vector<std::unique_ptr<BoundStatement>> Lowerer::LowerStatements(
    const core::BlockStatement& block) {
    std::vector<std::unique_ptr<BoundStatement>> statements;
    statements.reserve(block.Statements().size());
    for (const std::unique_ptr<core::Statement>& statement :
         block.Statements()) {
        statements.push_back(LowerStatement(*statement));
    }
    return statements;
}

std::unique_ptr<BoundBlockStatement> Lowerer::LowerBlock(
    const core::BlockStatement& block) {
    return std::make_unique<BoundBlockStatement>(block.Range(),
                                                 LowerStatements(block));
}

std::unique_ptr<BoundStatement> Lowerer::LowerStatement(
    const core::Statement& statement) {
    switch (statement.Kind()) {
        case core::AstNodeKind::RangeStatement:
            return LowerBlock(
                static_cast<const core::RangeStatement&>(statement)
                    .Expansion());
        case core::AstNodeKind::BlockStatement:
            return LowerBlock(
                static_cast<const core::BlockStatement&>(statement));
        case core::AstNodeKind::VariableDeclarationStatement: {
            const core::VariableDeclarationStatement& declaration =
                static_cast<const core::VariableDeclarationStatement&>(
                    statement);
            const semantic::VariableSymbol& symbol =
                static_cast<const semantic::VariableSymbol&>(
                    *_semanticModel->DeclaredSymbol(declaration));
            std::unique_ptr<BoundExpression> initializer;
            if (declaration.Initializer() != nullptr) {
                initializer =
                    LowerExpression(*declaration.Initializer(), symbol.Type());
            }
            return std::make_unique<BoundVariableDeclarationStatement>(
                declaration.Range(), symbol, std::move(initializer));
        }
        case core::AstNodeKind::DestructureStatement: {
            const auto& destructure =
                static_cast<const core::DestructureStatement&>(statement);
            std::vector<BoundDestructureBinding> bindings;
            for (const auto& binding : destructure.Bindings())
                bindings.push_back(
                    {static_cast<const semantic::VariableSymbol*>(
                         _semanticModel->DestructureTarget(*binding)),
                     _semanticModel->DeclaredSymbol(*binding) != nullptr});
            return std::make_unique<BoundDestructureStatement>(
                statement.Range(), std::move(bindings),
                LowerExpression(destructure.Value()));
        }
        case core::AstNodeKind::DeferStatement:
            return std::make_unique<BoundDeferStatement>(
                statement.Range(),
                LowerExpression(
                    static_cast<const core::DeferStatement&>(statement)
                        .Call()));
        case core::AstNodeKind::ExpressionStatement: {
            const core::ExpressionStatement& expressionStatement =
                static_cast<const core::ExpressionStatement&>(statement);
            return std::make_unique<BoundExpressionStatement>(
                statement.Range(),
                LowerExpression(expressionStatement.Value()));
        }
        case core::AstNodeKind::ReturnStatement: {
            const core::ReturnStatement& returnStatement =
                static_cast<const core::ReturnStatement&>(statement);
            std::unique_ptr<BoundExpression> expression;
            if (returnStatement.Value() != nullptr) {
                expression =
                    LowerExpression(*returnStatement.Value(), _returnType);
            }
            return std::make_unique<BoundReturnStatement>(
                statement.Range(), std::move(expression));
        }
        case core::AstNodeKind::IfStatement:
            return LowerIf(static_cast<const core::IfStatement&>(statement));
        case core::AstNodeKind::WhileStatement:
            return LowerWhile(
                static_cast<const core::WhileStatement&>(statement));
        case core::AstNodeKind::ProceedStatement:
            return std::make_unique<BoundProceedStatement>(statement.Range());
        case core::AstNodeKind::BreakStatement:
            return std::make_unique<BoundBreakStatement>(statement.Range());
        case core::AstNodeKind::JumpToStatement: {
            const core::JumpToStatement& jump =
                static_cast<const core::JumpToStatement&>(statement);
            return std::make_unique<BoundJumpToStatement>(statement.Range(),
                                                          jump.Position());
        }
        case core::AstNodeKind::PositionStatement: {
            const core::PositionStatement& position =
                static_cast<const core::PositionStatement&>(statement);
            return std::make_unique<BoundPositionStatement>(statement.Range(),
                                                            position.Name());
        }
        default:
            return nullptr;
    }
}

std::unique_ptr<BoundIfStatement> Lowerer::LowerBranch(
    const core::ConditionalBranch& branch,
    std::unique_ptr<BoundBlockStatement> elseBlock) {
    std::vector<std::unique_ptr<BoundStatement>> thenStatements =
        LowerStatements(branch.Body());
    if (branch.Also() != nullptr) {
        const core::AlsoClause& alsoClause = *branch.Also();
        thenStatements.push_back(std::make_unique<BoundIfStatement>(
            alsoClause.Range(), LowerExpression(alsoClause.Condition()),
            LowerBlock(alsoClause.Body()), nullptr));
    }
    const source::SourceLocation thenEnd = branch.Also() == nullptr
                                               ? branch.Body().Range().end
                                               : branch.Also()->Range().end;
    std::unique_ptr<BoundBlockStatement> thenBlock =
        std::make_unique<BoundBlockStatement>(
            source::SourceRange{branch.Body().Range().start, thenEnd},
            std::move(thenStatements));
    return std::make_unique<BoundIfStatement>(
        branch.Range(), LowerExpression(branch.Condition()),
        std::move(thenBlock), std::move(elseBlock));
}

std::unique_ptr<BoundStatement> Lowerer::LowerIf(
    const core::IfStatement& statement) {
    std::unique_ptr<BoundBlockStatement> alternative;
    if (statement.ElseBody() != nullptr) {
        alternative = LowerBlock(*statement.ElseBody());
    }

    const std::vector<std::unique_ptr<core::ConditionalBranch>>& branches =
        statement.ElseIfBranches();
    for (std::size_t index = branches.size(); index > 0; --index) {
        std::unique_ptr<BoundIfStatement> elseIf =
            LowerBranch(*branches[index - 1], std::move(alternative));
        std::vector<std::unique_ptr<BoundStatement>> statements;
        statements.push_back(std::move(elseIf));
        alternative = std::make_unique<BoundBlockStatement>(
            branches[index - 1]->Range(), std::move(statements));
    }
    return LowerBranch(statement.PrimaryBranch(), std::move(alternative));
}

std::unique_ptr<BoundStatement> Lowerer::LowerWhile(
    const core::WhileStatement& statement) {
    std::vector<std::unique_ptr<BoundStatement>> bodyStatements =
        LowerStatements(statement.Body());
    if (statement.Also() != nullptr) {
        const core::AlsoClause& alsoClause = *statement.Also();
        bodyStatements.push_back(std::make_unique<BoundIfStatement>(
            alsoClause.Range(), LowerExpression(alsoClause.Condition()),
            LowerBlock(alsoClause.Body()), nullptr));
    }
    const source::SourceLocation bodyEnd = statement.Also() == nullptr
                                               ? statement.Body().Range().end
                                               : statement.Also()->Range().end;
    std::unique_ptr<BoundBlockStatement> body =
        std::make_unique<BoundBlockStatement>(
            source::SourceRange{statement.Body().Range().start, bodyEnd},
            std::move(bodyStatements));
    return std::make_unique<BoundWhileStatement>(
        statement.Range(), LowerExpression(statement.Condition()),
        std::move(body),
        statement.Update() == nullptr ? nullptr
                                      : LowerExpression(*statement.Update()));
}

std::unique_ptr<BoundPlaceExpression> Lowerer::LowerPlace(
    const core::Expression& expression) {
    if (expression.Kind() == core::AstNodeKind::ParenthesizedExpression) {
        return LowerPlace(
            static_cast<const core::ParenthesizedExpression&>(expression)
                .Inner());
    }
    if (expression.Kind() == core::AstNodeKind::NameExpression) {
        const auto& name = static_cast<const core::NameExpression&>(expression);
        return std::make_unique<BoundVariableExpression>(
            expression.Range(), static_cast<const semantic::VariableSymbol&>(
                                    *_semanticModel->SymbolFor(name)));
    }
    if (expression.Kind() == core::AstNodeKind::MemberAccessExpression) {
        const auto& field =
            static_cast<const core::MemberAccessExpression&>(expression);
        const auto* resolved = _semanticModel->FieldFor(field);
        const auto receiverType = _semanticModel->TypeOf(field.Receiver());
        const auto* owner = field.Access() == TokenType::Arrow
                                ? receiverType.Target()->Structure()
                                : receiverType.Structure();
        const std::size_t index = resolved - owner->fields.data();
        return std::make_unique<BoundFieldExpression>(
            expression.Range(), resolved->type,
            LowerExpression(field.Receiver()), index,
            field.Access() == TokenType::Arrow);
    }
    if (expression.Kind() == core::AstNodeKind::MemberCallExpression) {
        const auto& call =
            static_cast<const core::MemberCallExpression&>(expression);
        return std::make_unique<BoundArrayElementExpression>(
            expression.Range(), LowerExpression(call.Receiver()), nullptr,
            call.Member() == "back");
    }
    if (expression.Kind() == core::AstNodeKind::IndexExpression) {
        const auto& index =
            static_cast<const core::IndexExpression&>(expression);
        return std::make_unique<BoundArrayElementExpression>(
            expression.Range(), LowerExpression(index.Receiver()),
            LowerExpression(index.Index()));
    }
    const auto& dereference =
        static_cast<const core::UnaryExpression&>(expression);
    return std::make_unique<BoundDereferenceExpression>(
        expression.Range(), LowerExpression(dereference.Operand()));
}

std::unique_ptr<BoundExpression> Lowerer::LowerExpression(
    const core::Expression& expression, semantic::SemanticType expected) {
    const semantic::SemanticType type = _semanticModel->TypeOf(expression);
    if (_semanticModel->TransferOf(expression) == semantic::TransferKind::Move)
        return std::make_unique<BoundMoveExpression>(expression.Range(),
                                                     LowerPlace(expression));
    if (_semanticModel->TransferOf(expression) ==
            semantic::TransferKind::Direct &&
        _directTransfer != &expression) {
        const core::Expression* previous = _directTransfer;
        _directTransfer = &expression;
        std::unique_ptr<BoundExpression> value =
            LowerExpression(expression, expected);
        _directTransfer = previous;
        return std::make_unique<BoundDirectExpression>(expression.Range(),
                                                       std::move(value));
    }
    switch (expression.Kind()) {
        case core::AstNodeKind::AnonymousFunctionExpression: {
            const auto& anonymous =
                static_cast<const core::AnonymousFunctionExpression&>(
                    expression);
            std::vector<BoundClosureCapture> captures;
            for (const semantic::ClosureCapture& capture :
                 _semanticModel->Captures(anonymous.Function())) {
                std::unique_ptr<BoundExpression> value =
                    std::make_unique<BoundVariableExpression>(
                        capture.syntax->NameRange(),
                        static_cast<const semantic::VariableSymbol&>(
                            *capture.source));
                if (capture.syntax->Mode() ==
                        core::AnonymousFunctionExpression::CaptureMode::Own &&
                    !capture.source->Type().IsCopyable())
                    value = std::make_unique<BoundMoveExpression>(
                        capture.syntax->NameRange(),
                        std::make_unique<BoundVariableExpression>(
                            capture.syntax->NameRange(),
                            static_cast<const semantic::VariableSymbol&>(
                                *capture.source)));
                captures.push_back({capture.syntax->Mode(), capture.variable,
                                    std::move(value)});
            }
            return std::make_unique<BoundFunctionExpression>(
                expression.Range(),
                static_cast<const semantic::FunctionSymbol&>(
                    *_semanticModel->DeclaredSymbol(anonymous.Function())),
                std::move(captures));
        }
        case core::AstNodeKind::BuildExpression: {
            const auto& build =
                static_cast<const core::BuildExpression&>(expression);
            std::vector<std::unique_ptr<BoundExpression>> defaults;
            for (const auto& field : type.Structure()->fields) {
                if (!_zeroFunction && field.syntax->Initializer())
                    defaults.push_back(LowerExpression(
                        *field.syntax->Initializer(), field.type));
                else
                    defaults.push_back(std::make_unique<BoundDefaultExpression>(
                        field.syntax->Range(), field.type, _zeroFunction));
            }
            return std::make_unique<BoundBuildExpression>(
                expression.Range(), type,
                static_cast<const semantic::VariableSymbol&>(
                    *_semanticModel->DeclaredSymbol(expression)),
                std::move(defaults), LowerBlock(build.Body()));
        }
        case core::AstNodeKind::SliceExpression: {
            const auto& slice =
                static_cast<const core::SliceExpression&>(expression);
            return std::make_unique<BoundSliceExpression>(
                expression.Range(), type, LowerExpression(slice.Receiver()),
                slice.Start() == nullptr ? nullptr
                                         : LowerExpression(*slice.Start()),
                slice.End() == nullptr ? nullptr
                                       : LowerExpression(*slice.End()));
        }
        case core::AstNodeKind::TupleExpression: {
            const auto& tuple =
                static_cast<const core::TupleExpression&>(expression);
            std::vector<std::unique_ptr<BoundExpression>> elements;
            for (std::size_t i = 0; i < tuple.Elements().size(); ++i)
                elements.push_back(
                    LowerExpression(*tuple.Elements()[i], type.Elements()[i]));
            return std::make_unique<BoundTupleExpression>(
                expression.Range(), type, std::move(elements));
        }
        case core::AstNodeKind::ArrayExpression: {
            const auto& array =
                static_cast<const core::ArrayExpression&>(expression);
            std::vector<std::unique_ptr<BoundExpression>> elements;
            for (const auto& element : array.Elements())
                elements.push_back(LowerExpression(*element, *type.Target()));
            return std::make_unique<BoundArrayExpression>(
                expression.Range(), type, std::move(elements),
                array.IsRepeat());
        }
        case core::AstNodeKind::MatchExpression: {
            const auto& match =
                static_cast<const core::MatchExpression&>(expression);
            const semantic::SemanticType matchedType =
                _semanticModel->TypeOf(match.Value());
            std::vector<BoundMatchArm> arms;
            arms.reserve(match.Arms().size());
            for (const auto& arm : match.Arms()) {
                std::optional<std::uint64_t> discriminant;
                if (!arm->IsWildcard()) {
                    for (const auto& variant : matchedType.Enum()->variants) {
                        if (variant.name == arm->Variant()) {
                            discriminant = variant.value;
                            break;
                        }
                    }
                }
                arms.push_back(
                    {discriminant, LowerExpression(arm->Value(), type)});
            }
            return std::make_unique<BoundMatchExpression>(
                expression.Range(), type, LowerExpression(match.Value()),
                std::move(arms));
        }
        case core::AstNodeKind::MemberAccessExpression: {
            const auto& member =
                static_cast<const core::MemberAccessExpression&>(expression);
            if (_semanticModel->FieldFor(member)) return LowerPlace(expression);
            if (type.Kind() == semantic::SemanticTypeKind::Enum) {
                for (const auto& variant : type.Enum()->variants)
                    if (variant.name == member.Member())
                        return std::make_unique<BoundLiteralExpression>(
                            expression.Range(), type,
                            std::to_string(variant.value));
            }
            const auto& variants = type.Family().variants;
            std::size_t code = 1;
            for (const auto& variant : variants) {
                if (variant == member.Member()) break;
                ++code;
            }
            return std::make_unique<BoundLiteralExpression>(
                expression.Range(), type, std::to_string(code));
        }
        case core::AstNodeKind::LiteralExpression: {
            const core::LiteralExpression& literal =
                static_cast<const core::LiteralExpression&>(expression);
            const auto containsEnum =
                [](const auto& self, semantic::SemanticType candidate) -> bool {
                if (candidate.Kind() == semantic::SemanticTypeKind::Enum)
                    return true;
                if (candidate.Kind() == semantic::SemanticTypeKind::Array)
                    return self(self, *candidate.Target());
                if (candidate.Kind() == semantic::SemanticTypeKind::Tuple)
                    for (const auto& element : candidate.Elements())
                        if (self(self, element)) return true;
                return false;
            };
            if (literal.LiteralType() == TokenType::None &&
                (expected.ContainsStructure() ||
                 containsEnum(containsEnum, expected))) {
                return std::make_unique<BoundDefaultExpression>(
                    expression.Range(), expected.WithoutConst(), true);
            }
            if ((literal.LiteralType() == TokenType::None ||
                 (literal.LiteralType() == TokenType::Null &&
                  (expected.Kind() == semantic::SemanticTypeKind::Pointer ||
                   expected.Kind() ==
                       semantic::SemanticTypeKind::OwnedBuffer))) &&
                !expected.IsError() &&
                expected.Kind() != semantic::SemanticTypeKind::None) {
                return std::make_unique<BoundLiteralExpression>(
                    expression.Range(), expected.WithoutConst(),
                    literal.Value(), true);
            }
            return std::make_unique<BoundLiteralExpression>(
                expression.Range(), type, literal.Value());
        }
        case core::AstNodeKind::NameExpression: {
            const core::NameExpression& name =
                static_cast<const core::NameExpression&>(expression);
            const semantic::Symbol* symbol = _semanticModel->SymbolFor(name);
            if (symbol->Kind() == semantic::SymbolKind::Function) {
                return std::make_unique<BoundFunctionExpression>(
                    expression.Range(),
                    static_cast<const semantic::FunctionSymbol&>(*symbol));
            }
            return std::make_unique<BoundVariableExpression>(
                expression.Range(),
                static_cast<const semantic::VariableSymbol&>(*symbol));
        }
        case core::AstNodeKind::ParenthesizedExpression:
            return LowerExpression(
                static_cast<const core::ParenthesizedExpression&>(expression)
                    .Inner(),
                expected);
        case core::AstNodeKind::CastExpression: {
            const core::CastExpression& cast =
                static_cast<const core::CastExpression&>(expression);
            return std::make_unique<BoundCastExpression>(
                expression.Range(), type, LowerExpression(cast.Value()));
        }
        case core::AstNodeKind::UnaryExpression: {
            const core::UnaryExpression& unary =
                static_cast<const core::UnaryExpression&>(expression);
            if (unary.OperatorType() == TokenType::Increment)
                return std::make_unique<BoundIncrementExpression>(
                    expression.Range(), LowerPlace(unary.Operand()));
            if (unary.OperatorType() == TokenType::Mul)
                return LowerPlace(expression);
            if (unary.OperatorType() == TokenType::RawAddress ||
                unary.OperatorType() == TokenType::Ampersand) {
                return std::make_unique<BoundAddressExpression>(
                    expression.Range(), type, LowerPlace(unary.Operand()));
            }
            return std::make_unique<BoundUnaryExpression>(
                expression.Range(), type, unary.OperatorType(),
                LowerExpression(unary.Operand()));
        }
        case core::AstNodeKind::BinaryExpression: {
            const core::BinaryExpression& binary =
                static_cast<const core::BinaryExpression&>(expression);
            const semantic::SemanticType leftType =
                _semanticModel->TypeOf(binary.Left());
            const semantic::SemanticType rightType =
                _semanticModel->TypeOf(binary.Right());
            const bool valueEquality =
                binary.OperatorType() == TokenType::Equal ||
                binary.OperatorType() == TokenType::NotEqual;
            const auto comparisonContext = [&](semantic::SemanticType other) {
                const auto kind = other.Kind();
                // Ordinary equality interprets none as the other operand's
                // zero value. Strict equality retains none's distinct type.
                if ((valueEquality &&
                     (other.IsNumeric() || other.IsErrorValue() ||
                      other.ContainsStructure() ||
                      kind == semantic::SemanticTypeKind::Enum ||
                      kind == semantic::SemanticTypeKind::Bool ||
                      kind == semantic::SemanticTypeKind::Char)) ||
                    kind == semantic::SemanticTypeKind::Pointer ||
                    kind == semantic::SemanticTypeKind::OwnedBuffer ||
                    kind == semantic::SemanticTypeKind::Null)
                    return other;
                return semantic::SemanticType::Error();
            };
            return std::make_unique<BoundBinaryExpression>(
                expression.Range(), type,
                LowerExpression(binary.Left(), comparisonContext(rightType)),
                binary.OperatorType(),
                LowerExpression(binary.Right(), comparisonContext(leftType)));
        }
        case core::AstNodeKind::AssignmentExpression: {
            const core::AssignmentExpression& assignment =
                static_cast<const core::AssignmentExpression&>(expression);
            std::unique_ptr<BoundPlaceExpression> target =
                LowerPlace(assignment.Target());
            const semantic::SemanticType targetType = target->Type();
            return std::make_unique<BoundAssignmentExpression>(
                expression.Range(), std::move(target),
                LowerExpression(assignment.Value(), targetType));
        }
        case core::AstNodeKind::CallExpression: {
            const core::CallExpression& call =
                static_cast<const core::CallExpression&>(expression);
            const semantic::FunctionSymbol* function = nullptr;
            if (call.Callee().Kind() == core::AstNodeKind::NameExpression) {
                const auto& name =
                    static_cast<const core::NameExpression&>(call.Callee());
                const auto* symbol = _semanticModel->SymbolFor(name);
                if (symbol && symbol->Kind() == semantic::SymbolKind::Function)
                    function =
                        static_cast<const semantic::FunctionSymbol*>(symbol);
            }
            const auto& signature =
                _semanticModel->TypeOf(call.Callee()).Signature();
            if (function &&
                function->BuiltIn() == semantic::BuiltInFunction::New)
                return std::make_unique<BoundAllocationExpression>(
                    expression.Range(), type, *type.Elements()[0].Target(),
                    _semanticModel->GlobalScope()
                        .Lookup("AllocationError")
                        ->Type(),
                    LowerExpression(*call.Arguments()[0],
                                    function->ParameterTypes()[0]),
                    call.TypeArgument()->TypeForm() == core::TypeKind::Slice);
            std::vector<std::unique_ptr<BoundExpression>> arguments;
            arguments.reserve(call.Arguments().size());
            for (const std::unique_ptr<core::Expression>& argument :
                 call.Arguments()) {
                const semantic::SemanticType parameterType =
                    (!function ||
                     function->BuiltIn() == semantic::BuiltInFunction::None ||
                     function->BuiltIn() == semantic::BuiltInFunction::Free)
                        ? signature.parameters[arguments.size()]
                        : semantic::SemanticType::Error();
                arguments.push_back(LowerExpression(*argument, parameterType));
            }
            if (!function)
                return std::make_unique<BoundIndirectCallExpression>(
                    expression.Range(), LowerExpression(call.Callee()),
                    std::move(arguments));
            return std::make_unique<BoundCallExpression>(
                expression.Range(), *function, std::move(arguments));
        }
        case core::AstNodeKind::MemberCallExpression: {
            const core::MemberCallExpression& member =
                static_cast<const core::MemberCallExpression&>(expression);
            if (const auto* method = _semanticModel->MethodFor(member)) {
                std::vector<std::unique_ptr<BoundExpression>> arguments;
                if (method->HasReceiver()) {
                    if (_semanticModel->TypeOf(member.Receiver()).Kind() ==
                            semantic::SemanticTypeKind::Structure ||
                        _semanticModel->TypeOf(member.Receiver()).Kind() ==
                            semantic::SemanticTypeKind::Enum)
                        arguments.push_back(
                            std::make_unique<BoundAddressExpression>(
                                member.Receiver().Range(),
                                method->ParameterTypes()[0],
                                LowerPlace(member.Receiver())));
                    else
                        arguments.push_back(LowerExpression(member.Receiver()));
                }
                for (const auto& argument : member.Arguments())
                    arguments.push_back(LowerExpression(
                        *argument, method->ParameterTypes()[arguments.size()]));
                return std::make_unique<BoundCallExpression>(
                    expression.Range(), *method, std::move(arguments));
            }
            if (_semanticModel->TypeOf(member.Receiver()).IsSequence() &&
                member.Member() != "len")
                return LowerPlace(expression);
            if (_semanticModel->TypeOf(member.Receiver()).IsSequence())
                return std::make_unique<BoundArrayLengthExpression>(
                    expression.Range(), LowerExpression(member.Receiver()));
            StringViewOperation operation = StringViewOperation::Length;
            if (member.Member() == "at") {
                operation = StringViewOperation::At;
            } else if (member.Member() == "substr") {
                operation = StringViewOperation::Substring;
            } else if (member.Member() == "string") {
                operation = StringViewOperation::ToString;
            } else if (member.Member() == "view") {
                operation = StringViewOperation::View;
            } else if (member.Member() == "copy") {
                operation = StringViewOperation::Copy;
            } else if (member.Member() == "data") {
                operation = StringViewOperation::Data;
            }
            std::vector<std::unique_ptr<BoundExpression>> arguments;
            arguments.reserve(member.Arguments().size());
            for (const std::unique_ptr<core::Expression>& argument :
                 member.Arguments()) {
                arguments.push_back(LowerExpression(*argument));
            }
            return std::make_unique<BoundStringViewOperationExpression>(
                expression.Range(), type, operation,
                LowerExpression(member.Receiver()), std::move(arguments));
        }
        case core::AstNodeKind::IndexExpression: {
            const core::IndexExpression& index =
                static_cast<const core::IndexExpression&>(expression);
            if (_semanticModel->TypeOf(index.Receiver()).IsSequence() ||
                _semanticModel->TypeOf(index.Receiver()).Kind() ==
                    semantic::SemanticTypeKind::Pointer)
                return LowerPlace(expression);
            std::vector<std::unique_ptr<BoundExpression>> arguments;
            arguments.push_back(LowerExpression(index.Index()));
            return std::make_unique<BoundStringViewOperationExpression>(
                expression.Range(), type, StringViewOperation::At,
                LowerExpression(index.Receiver()), std::move(arguments));
        }
        default:
            return nullptr;
    }
}

}  // namespace flecha::lowering
