#include "semantic/control_flow_graph.hpp"

#include <utility>
namespace flecha::semantic {
namespace {
class GraphBuilder final {
   public:
    explicit GraphBuilder(const SemanticModel& model) : _model(model) {}
    ControlFlowGraph Build(const core::FunctionDeclaration& function) {
        const NodeId exit = AddNode({});
        _graph.exit = exit;
        const NodeId body = BuildBlock(function.Body(), exit);
        _graph.entry = AddNode({body});
        for (const auto& [jump, position] : _jumps) {
            const auto target = _positions.find(position);
            if (target != _positions.end()) {
                _graph.nodes[jump].successors.push_back(target->second);
            }
        }
        for (NodeId node = 0; node < _graph.nodes.size(); ++node) {
            for (NodeId successor : _graph.nodes[node].successors) {
                _graph.nodes[successor].predecessors.push_back(node);
            }
        }

        std::vector<NodeId> pending{_graph.entry};
        while (!pending.empty()) {
            const NodeId node = pending.back();
            pending.pop_back();
            if (_graph.nodes[node].reachable) continue;
            _graph.nodes[node].reachable = true;
            for (NodeId successor : _graph.nodes[node].successors) {
                pending.push_back(successor);
            }
        }

        return std::move(_graph);
    }

   private:
    NodeId AddNode(std::vector<NodeId> successors) {
        const NodeId node = _graph.nodes.size();
        _graph.nodes.push_back(FlowNode{});
        _graph.nodes.back().successors = std::move(successors);
        _graph.nodes.back().scopes = _scopes;
        return node;
    }

    NodeId BuildBlock(const core::BlockStatement& block, NodeId next,
                      const core::AlsoClause* also = nullptr) {
        _graph.scopeParents[&block] =
            _scopes.empty() ? nullptr : _scopes.back();
        _scopes.push_back(&block);
        // Keep the body's scope active through its trailing also clause,
        // matching lowering's combined block. The also body adds its own scope;
        // name binding still keeps the two bodies' declarations separate.
        const NodeId end = AddNode({next});
        next = end;
        if (also != nullptr) {
            const NodeId body = BuildBlock(also->Body(), next);
            next = BuildCondition(also->Condition(), body, next);
        }
        for (auto statement = block.Statements().rbegin();
             statement != block.Statements().rend(); ++statement) {
            next = BuildStatement(**statement, next);
        }
        _scopes.pop_back();
        return next;
    }

    NodeId BuildBranch(const core::ConditionalBranch& branch, NodeId next,
                       NodeId alternative) {
        const NodeId body = BuildBlock(branch.Body(), next, branch.Also());
        return BuildCondition(branch.Condition(), body, alternative);
    }

    NodeId BuildStatement(const core::Statement& statement, NodeId next) {
        switch (statement.Kind()) {
            case core::AstNodeKind::RangeStatement:
                return BuildBlock(
                    static_cast<const core::RangeStatement&>(statement)
                        .Expansion(),
                    next);
            case core::AstNodeKind::BlockStatement:
                return BuildBlock(
                    static_cast<const core::BlockStatement&>(statement), next);
            case core::AstNodeKind::VariableDeclarationStatement: {
                const auto& declaration =
                    static_cast<const core::VariableDeclarationStatement&>(
                        statement);
                const Symbol* symbol = _model.DeclaredSymbol(declaration);
                if (symbol != nullptr)
                    _graph.symbolScopes.emplace(symbol, _scopes.back());
                const NodeId store = AddNode({next});
                _graph.nodes[store].reset = symbol;
                _graph.nodes[store].memoryDeclaration = &declaration;
                NodeId entry = store;
                if (declaration.Initializer() != nullptr) {
                    _graph.nodes[store].write = symbol;
                    entry = BuildExpression(*declaration.Initializer(), store);
                }
                const NodeId action = AddNode({entry});
                _graph.nodes[action].declaration = &declaration;
                return action;
            }
            case core::AstNodeKind::DestructureStatement: {
                const auto& destructure =
                    static_cast<const core::DestructureStatement&>(statement);
                for (auto binding = destructure.Bindings().rbegin();
                     binding != destructure.Bindings().rend(); ++binding) {
                    const Symbol* target = _model.DestructureTarget(**binding);
                    if (target == nullptr) continue;
                    const NodeId store = AddNode({next});
                    _graph.nodes[store].write = target;
                    if (_model.DeclaredSymbol(**binding) != nullptr) {
                        _graph.nodes[store].reset = target;
                        _graph.symbolScopes.emplace(target, _scopes.back());
                    }
                    next = store;
                }
                const NodeId memoryStore = AddNode({next});
                _graph.nodes[memoryStore].memoryDestructure = &destructure;
                const NodeId body =
                    BuildExpression(destructure.Value(), memoryStore);
                const NodeId action = AddNode({body});
                _graph.nodes[action].destructure = &destructure;
                return action;
            }
            case core::AstNodeKind::DeferStatement: {
                const auto& deferred =
                    static_cast<const core::DeferStatement&>(statement);
                for (const Symbol* capture :
                     _model.DeferredCaptures(deferred)) {
                    if (capture == nullptr) continue;
                    _graph.symbolScopes.emplace(capture, _scopes.back());
                    _graph.deferredCaptures.insert(capture);
                }
                const NodeId registration = AddNode({next});
                _graph.nodes[registration].memoryDefer = &deferred;
                const NodeId body =
                    BuildExpressionBody(deferred.Call(), registration);
                const NodeId action = AddNode({body});
                _graph.nodes[action].deferRegistration = &deferred;
                return action;
            }
            case core::AstNodeKind::ExpressionStatement: {
                const auto& value =
                    static_cast<const core::ExpressionStatement&>(statement)
                        .Value();
                const NodeId discard = AddNode({next});
                _graph.nodes[discard].memoryDiscard = &value;
                return BuildEvaluation(value, discard);
            }
            case core::AstNodeKind::ReturnStatement: {
                const auto& result =
                    static_cast<const core::ReturnStatement&>(statement);
                const NodeId exit = AddNode({});
                _graph.nodes[exit].memoryReturn = &result;
                return result.Value() == nullptr
                           ? exit
                           : BuildEvaluation(*result.Value(), exit, &result);
            }
            case core::AstNodeKind::IfStatement: {
                const auto& conditional =
                    static_cast<const core::IfStatement&>(statement);
                NodeId alternative =
                    conditional.ElseBody() == nullptr
                        ? next
                        : BuildBlock(*conditional.ElseBody(), next);
                for (auto branch = conditional.ElseIfBranches().rbegin();
                     branch != conditional.ElseIfBranches().rend(); ++branch) {
                    alternative = BuildBranch(**branch, next, alternative);
                }
                return BuildBranch(conditional.PrimaryBranch(), next,
                                   alternative);
            }
            case core::AstNodeKind::WhileStatement: {
                const auto& loop =
                    static_cast<const core::WhileStatement&>(statement);
                const NodeId header = AddNode({});
                const NodeId update =
                    loop.Update() == nullptr
                        ? header
                        : BuildEvaluation(*loop.Update(), header);
                _loops.push_back({update, next});
                const NodeId body =
                    BuildBlock(loop.Body(), update, loop.Also());
                _loops.pop_back();
                const NodeId condition =
                    BuildCondition(loop.Condition(), body, next);
                _graph.nodes[header].successors.push_back(condition);
                return header;
            }
            case core::AstNodeKind::ProceedStatement:
                return AddNode(_loops.empty()
                                   ? std::vector<NodeId>{}
                                   : std::vector<NodeId>{_loops.back().first});
            case core::AstNodeKind::BreakStatement:
                return AddNode(_loops.empty()
                                   ? std::vector<NodeId>{}
                                   : std::vector<NodeId>{_loops.back().second});
            case core::AstNodeKind::JumpToStatement: {
                const NodeId jump = AddNode({});
                _jumps.emplace_back(
                    jump, static_cast<const core::JumpToStatement&>(statement)
                              .Position());
                return jump;
            }
            case core::AstNodeKind::PositionStatement: {
                const NodeId position = AddNode({next});
                _positions.emplace(
                    static_cast<const core::PositionStatement&>(statement)
                        .Name(),
                    position);
                return position;
            }
            default:
                return next;
        }
    }

    NodeId BuildCondition(const core::Expression& expression, NodeId yes,
                          NodeId no, bool trackBorrows = true) {
        if (expression.Kind() == core::AstNodeKind::ParenthesizedExpression) {
            return BuildCondition(
                static_cast<const core::ParenthesizedExpression&>(expression)
                    .Inner(),
                yes, no, trackBorrows);
        }
        if (expression.Kind() == core::AstNodeKind::UnaryExpression) {
            const auto& unary =
                static_cast<const core::UnaryExpression&>(expression);
            if (unary.OperatorType() == TokenType::Bang) {
                return BuildCondition(unary.Operand(), no, yes, trackBorrows);
            }
        }
        if (expression.Kind() == core::AstNodeKind::BinaryExpression &&
            _model.TypeOf(expression).Kind() == SemanticTypeKind::Bool) {
            const auto& binary =
                static_cast<const core::BinaryExpression&>(expression);
            if (binary.OperatorType() == TokenType::And) {
                const NodeId right =
                    BuildCondition(binary.Right(), yes, no, trackBorrows);
                return BuildCondition(binary.Left(), right, no, trackBorrows);
            }
            if (binary.OperatorType() == TokenType::Or) {
                const NodeId right =
                    BuildCondition(binary.Right(), yes, no, trackBorrows);
                return BuildCondition(binary.Left(), yes, right, trackBorrows);
            }
        }
        // Deliberately keep both outcomes, including literal conditions. This
        // is definite assignment, not constant folding or value-range analysis.
        const NodeId branch = AddNode({yes, no});
        _graph.nodes[branch].condition = &expression;
        return trackBorrows ? BuildEvaluation(expression, branch)
                            : BuildExpression(expression, branch);
    }

    NodeId BuildEvaluation(
        const core::Expression& expression, NodeId next,
        const core::ReturnStatement* borrowReturn = nullptr) {
        const NodeId body = BuildExpression(expression, next);
        const NodeId action = AddNode({body});
        _graph.nodes[action].evaluation = &expression;
        _graph.nodes[action].borrowReturn = borrowReturn;
        return action;
    }

    NodeId BuildExpression(const core::Expression& expression, NodeId next) {
        const NodeId result = AddNode({next});
        _graph.nodes[result].memoryExpression = &expression;
        return BuildExpressionBody(expression, result);
    }

    NodeId BuildExpressionBody(const core::Expression& expression,
                               NodeId next) {
        switch (expression.Kind()) {
            case core::AstNodeKind::AnonymousFunctionExpression: {
                const auto& function =
                    static_cast<const core::AnonymousFunctionExpression&>(
                        expression);
                for (auto capture = function.Captures().rbegin();
                     capture != function.Captures().rend(); ++capture) {
                    const NodeId read = AddNode({next});
                    _graph.nodes[read].captureRead = capture->get();
                    const Symbol* source = _model.CaptureSource(**capture);
                    if ((*capture)->Mode() ==
                            core::AnonymousFunctionExpression::CaptureMode::
                                Own &&
                        source != nullptr && !source->Type().IsCopyable() &&
                        source->Type().Kind() != SemanticTypeKind::Pointer)
                        _graph.nodes[read].move = source;
                    next = read;
                }
                return next;
            }
            case core::AstNodeKind::BuildExpression: {
                const auto& build =
                    static_cast<const core::BuildExpression&>(expression);
                const Symbol* self = _model.DeclaredSymbol(expression);
                // Construction self is a temporary capability. A reference to
                // one of its fields must not escape and bypass loans on the
                // completed destination variable.
                _graph.symbolScopes[self] = &build.Body();
                const NodeId body = BuildBlock(build.Body(), next);
                const NodeId start = AddNode({body});
                _graph.nodes[start].reset = self;
                _graph.nodes[start].write = self;
                NodeId defaults = start;
                const StructureType* structure =
                    _model.TypeOf(expression).Structure();
                if (structure) {
                    for (auto field = structure->fields.rbegin();
                         field != structure->fields.rend(); ++field) {
                        if (!field->syntax->Initializer()) continue;
                        const NodeId store = AddNode({defaults});
                        _graph.nodes[store].memoryUntrackedStore =
                            field->syntax->Initializer();
                        defaults = BuildExpression(
                            *field->syntax->Initializer(), store);
                    }
                }
                return defaults;
            }
            case core::AstNodeKind::MemberAccessExpression: {
                const auto& field =
                    static_cast<const core::MemberAccessExpression&>(
                        expression);
                return _model.FieldFor(field)
                           ? BuildExpression(field.Receiver(), next)
                           : next;
            }
            case core::AstNodeKind::SliceExpression: {
                const auto& slice =
                    static_cast<const core::SliceExpression&>(expression);
                if (slice.End() != nullptr)
                    next = BuildExpression(*slice.End(), next);
                if (slice.Start() != nullptr)
                    next = BuildExpression(*slice.Start(), next);
                return BuildExpression(slice.Receiver(), next);
            }
            case core::AstNodeKind::TupleExpression: {
                const auto& tuple =
                    static_cast<const core::TupleExpression&>(expression);
                for (auto element = tuple.Elements().rbegin();
                     element != tuple.Elements().rend(); ++element)
                    next = BuildExpression(**element, next);
                return next;
            }
            case core::AstNodeKind::ArrayExpression: {
                const auto& array =
                    static_cast<const core::ArrayExpression&>(expression);
                for (auto element = array.Elements().rbegin();
                     element != array.Elements().rend(); ++element)
                    next = BuildExpression(**element, next);
                return next;
            }
            case core::AstNodeKind::MatchExpression: {
                const auto& match =
                    static_cast<const core::MatchExpression&>(expression);
                std::vector<NodeId> alternatives;
                alternatives.reserve(match.Arms().size());
                for (const auto& arm : match.Arms())
                    alternatives.push_back(BuildExpression(arm->Value(), next));
                const NodeId dispatch = AddNode(std::move(alternatives));
                return BuildExpression(match.Value(), dispatch);
            }
            case core::AstNodeKind::NameExpression: {
                if (_model.TransferOf(expression) == TransferKind::Move) {
                    const NodeId move = AddNode({next});
                    _graph.nodes[move].move = _model.SymbolFor(
                        static_cast<const core::NameExpression&>(expression));
                    next = move;
                }
                const NodeId read = AddNode({next});
                _graph.nodes[read].read =
                    static_cast<const core::NameExpression*>(&expression);
                return read;
            }
            case core::AstNodeKind::ParenthesizedExpression:
                return BuildExpression(
                    static_cast<const core::ParenthesizedExpression&>(
                        expression)
                        .Inner(),
                    next);
            case core::AstNodeKind::UnaryExpression:
                return BuildExpression(
                    static_cast<const core::UnaryExpression&>(expression)
                        .Operand(),
                    next);
            case core::AstNodeKind::CastExpression:
                return BuildExpression(
                    static_cast<const core::CastExpression&>(expression)
                        .Value(),
                    next);
            case core::AstNodeKind::BinaryExpression: {
                const auto& binary =
                    static_cast<const core::BinaryExpression&>(expression);
                if (_model.TypeOf(expression).Kind() ==
                        SemanticTypeKind::Bool &&
                    (binary.OperatorType() == TokenType::And ||
                     binary.OperatorType() == TokenType::Or)) {
                    const NodeId yes = AddNode({next});
                    _graph.nodes[yes].memoryBoolean = &expression;
                    _graph.nodes[yes].booleanValue = true;
                    const NodeId no = AddNode({next});
                    _graph.nodes[no].memoryBoolean = &expression;
                    return BuildCondition(expression, yes, no, false);
                }
                const NodeId right = BuildExpression(binary.Right(), next);
                return BuildExpression(binary.Left(), right);
            }
            case core::AstNodeKind::AssignmentExpression: {
                const auto& assignment =
                    static_cast<const core::AssignmentExpression&>(expression);
                const NodeId store = AddNode({next});
                const core::Expression* target = &assignment.Target();
                while (target->Kind() ==
                       core::AstNodeKind::ParenthesizedExpression) {
                    target = &static_cast<const core::ParenthesizedExpression&>(
                                  *target)
                                  .Inner();
                }
                if (target->Kind() == core::AstNodeKind::NameExpression) {
                    _graph.nodes[store].write = _model.SymbolFor(
                        static_cast<const core::NameExpression&>(*target));
                    const NodeId value =
                        BuildExpression(assignment.Value(), store);
                    if (assignment.OperatorType() != TokenType::Assign)
                        return BuildExpression(*target, value);
                    return value;
                }
                // Indirect stores read the pointer, but do not initialize its
                // binding or prove another local initialized through aliasing.
                const NodeId value = BuildExpression(assignment.Value(), store);
                return BuildExpression(*target, value);
            }
            case core::AstNodeKind::CallExpression: {
                const auto& call =
                    static_cast<const core::CallExpression&>(expression);
                const auto* callee =
                    call.Callee().Kind() == core::AstNodeKind::NameExpression
                        ? _model.SymbolFor(
                              static_cast<const core::NameExpression&>(
                                  call.Callee()))
                        : nullptr;
                const auto* function =
                    callee && callee->Kind() == SymbolKind::Function
                        ? static_cast<const FunctionSymbol*>(callee)
                        : nullptr;
                const bool allocation =
                    function &&
                    (function->BuiltIn() == BuiltInFunction::New ||
                     function->BuiltIn() == BuiltInFunction::StorageNew);
                const bool dynamic =
                    allocation && call.TypeArgument() &&
                    call.TypeArgument()->TypeForm() == core::TypeKind::Slice;
                const NodeId failure = next;
                if (dynamic) {
                    next = AddNode({next, failure});
                    _graph.nodes[next].allocation = &call;
                }
                for (auto argument = call.Arguments().rbegin();
                     argument != call.Arguments().rend(); ++argument) {
                    next = BuildExpression(**argument, next);
                }
                if (allocation && call.TypeArgument() && !dynamic) {
                    next = AddNode({next, failure});
                    _graph.nodes[next].allocation = &call;
                }
                return BuildExpression(call.Callee(), next);
            }
            case core::AstNodeKind::MemberCallExpression: {
                const auto& call =
                    static_cast<const core::MemberCallExpression&>(expression);
                for (auto argument = call.Arguments().rbegin();
                     argument != call.Arguments().rend(); ++argument) {
                    next = BuildExpression(**argument, next);
                }
                return call.Access() == TokenType::Scope
                           ? next
                           : BuildExpression(call.Receiver(), next);
            }
            case core::AstNodeKind::IndexExpression: {
                const auto& index =
                    static_cast<const core::IndexExpression&>(expression);
                const NodeId subscript = BuildExpression(index.Index(), next);
                return BuildExpression(index.Receiver(), subscript);
            }
            default:
                return next;
        }
    }

    const SemanticModel& _model;
    ControlFlowGraph _graph;
    std::vector<const core::BlockStatement*> _scopes;
    std::unordered_map<std::string, NodeId> _positions;
    std::vector<std::pair<NodeId, std::string>> _jumps;
    std::vector<std::pair<NodeId, NodeId>> _loops;
};
}  // namespace
ControlFlowGraph BuildControlFlowGraph(
    const core::FunctionDeclaration& function, const SemanticModel& model) {
    return GraphBuilder(model).Build(function);
}

bool CanReachFunctionExit(const ControlFlowGraph& graph) {
    std::vector<NodeId> pending{graph.entry};
    std::unordered_set<NodeId> visited;
    while (!pending.empty()) {
        const NodeId id = pending.back();
        pending.pop_back();
        if (!visited.insert(id).second) continue;
        if (id == graph.exit) return true;

        const FlowNode& node = graph.nodes[id];
        if (node.condition != nullptr && node.successors.size() == 2 &&
            node.condition->Kind() == core::AstNodeKind::LiteralExpression) {
            const auto& literal =
                static_cast<const core::LiteralExpression&>(*node.condition);
            if (literal.LiteralType() == TokenType::True) {
                pending.push_back(node.successors[0]);
                continue;
            }
            if (literal.LiteralType() == TokenType::False) {
                pending.push_back(node.successors[1]);
                continue;
            }
        }
        pending.insert(pending.end(), node.successors.begin(),
                       node.successors.end());
    }
    return false;
}
}  // namespace flecha::semantic
