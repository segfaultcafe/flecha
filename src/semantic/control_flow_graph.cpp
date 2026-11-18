#include "semantic/control_flow_graph.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <utility>

#include "core/ast_walk.hpp"
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
        SpecializeCheckedPaths();
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

        if (function.IsAsync()) PlanTaskScopeExits();
        return std::move(_graph);
    }

   private:
    bool HasTry(const core::Expression& expression) const {
        bool found = false;
        unsigned functions = 0;
        core::WalkAst(expression, [&](core::AstNode& node, bool entering) {
            if (node.Kind() == core::AstNodeKind::AnonymousFunctionExpression) {
                if (entering)
                    ++functions;
                else
                    --functions;
            }
            if (entering && functions == 0 &&
                node.Kind() == core::AstNodeKind::TryExpression)
                found = true;
        });
        return found;
    }

    NodeId BuildTryAction(const core::Expression& expression, NodeId action,
                          bool deferred = false) {
        const auto* previous = _tryEvaluation;
        _tryEvaluation = &expression;
        const NodeId body = deferred ? BuildExpressionBody(expression, action)
                                     : BuildExpression(expression, action);
        _tryEvaluation = previous;
        return body;
    }

    void PlanTaskScopeExits() {
        for (NodeId id = 0; id < _graph.nodes.size(); ++id) {
            const FlowNode& node = _graph.nodes[id];
            if (!node.reachable) continue;
            if (node.memoryReturn || node.failureExit ||
                node.propagationReturn) {
                _graph.taskScopeExits.push_back(
                    {id,
                     std::nullopt,
                     TaskScopeExitKind::CancelAndDrain,
                     {node.scopes.rbegin(), node.scopes.rend()}});
                continue;
            }
            // Cross-scope jumps are diagnosed separately, never treated as
            // implicit permission to enter or abandon a task scope.
            if (node.transfer &&
                node.transfer->Kind() == core::AstNodeKind::JumpToStatement)
                continue;
            for (NodeId successor : node.successors) {
                const auto& target = _graph.nodes[successor].scopes;
                std::size_t common = 0;
                while (common < node.scopes.size() && common < target.size() &&
                       node.scopes[common] == target[common])
                    ++common;
                if (common == node.scopes.size()) continue;
                TaskScopeExit exit{id,
                                   successor,
                                   node.transfer
                                       ? TaskScopeExitKind::CancelAndDrain
                                       : TaskScopeExitKind::Join,
                                   {}};
                for (std::size_t index = node.scopes.size(); index > common;
                     --index)
                    exit.scopes.push_back(node.scopes[index - 1]);
                _graph.taskScopeExits.push_back(std::move(exit));
            }
        }
    }

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

    NodeId BuildPeekBlock(const core::TryExpression& attempt, NodeId next) {
        const core::BlockStatement& block = *attempt.PeekBody();
        _graph.scopeParents[&block] =
            _scopes.empty() ? nullptr : _scopes.back();
        _scopes.push_back(&block);
        const NodeId end = AddNode({next});
        next = end;
        for (auto statement = block.Statements().rbegin();
             statement != block.Statements().rend(); ++statement)
            next = BuildStatement(**statement, next);
        if (const auto* binding = attempt.PeekBinding()) {
            const Symbol* symbol = _model.DeclaredSymbol(*binding);
            if (symbol != nullptr) _graph.symbolScopes.emplace(symbol, &block);
            const NodeId initialize = AddNode({next});
            _graph.nodes[initialize].reset = symbol;
            _graph.nodes[initialize].write = symbol;
            _graph.nodes[initialize].peekInitialization = &attempt;
            next = initialize;
        }
        _scopes.pop_back();
        return next;
    }

    static const core::Expression& Unwrap(const core::Expression& expression) {
        if (expression.Kind() == core::AstNodeKind::ParenthesizedExpression)
            return Unwrap(
                static_cast<const core::ParenthesizedExpression&>(expression)
                    .Inner());
        return expression;
    }

    NodeId BuildDestructure(const core::DestructureStatement& destructure,
                            NodeId next, bool success) {
        const auto* checked = _model.CheckedResultFor(destructure);
        for (auto binding = destructure.Bindings().rbegin();
             binding != destructure.Bindings().rend(); ++binding) {
            const Symbol* target = _model.DestructureTarget(**binding);
            if (!target) continue;
            const NodeId store = AddNode({next});
            _graph.nodes[store].checkedInitialization = checked != nullptr;
            if (!checked || success || target == checked->error)
                _graph.nodes[store].write = target;
            if (_model.DeclaredSymbol(**binding)) {
                _graph.nodes[store].reset = target;
                _graph.symbolScopes.emplace(target, _scopes.back());
            }
            next = store;
        }
        const NodeId memoryStore = AddNode({next});
        _graph.nodes[memoryStore].memoryDestructure = &destructure;
        // Creation failure bypasses receiver/argument evaluation completely.
        // Reservation is represented by the checked dispatch itself; only the
        // success continuation evaluates and transfers prepared operands.
        if (checked && checked->creation && !success) {
            const NodeId action = AddNode({memoryStore});
            _graph.nodes[action].destructure = &destructure;
            _graph.nodes[action].asyncSuccess = false;
            return action;
        }
        if (HasTry(destructure.Value())) {
            const NodeId action = AddNode({memoryStore});
            _graph.nodes[action].destructure = &destructure;
            _graph.nodes[action].asyncSuccess = success;
            const NodeId body = BuildTryAction(destructure.Value(), action);
            return body;
        }
        const NodeId body = BuildExpression(destructure.Value(), memoryStore);
        const NodeId action = AddNode({body});
        _graph.nodes[action].destructure = &destructure;
        _graph.nodes[action].asyncSuccess = success;
        return action;
    }

    // Preserve correlations only while their error binding is live. A checked
    // result is not an always-initialized tuple: merging success and failure
    // before the check would lose both definite assignment and ownership facts.
    // Specialization shares identical (node, live outcomes) states, including
    // loop backedges, and leaves all ordinary CFGs unchanged.
    void SpecializeCheckedPaths() {
        using Outcomes = std::map<const Symbol*, bool>;
        std::set<const Symbol*> errors;
        for (const auto& node : _graph.nodes)
            if (node.destructure)
                if (const auto* checked =
                        _model.CheckedResultFor(*node.destructure))
                    errors.insert(checked->error);
        if (errors.empty()) return;
        const auto original = std::move(_graph.nodes);
        _graph.nodes.clear();
        std::vector<std::set<const Symbol*>> live(original.size());
        bool changed = true;
        while (changed) {
            changed = false;
            for (NodeId id = 0; id < original.size(); ++id) {
                auto next = std::set<const Symbol*>{};
                for (auto successor : original[id].successors)
                    next.insert(live[successor].begin(), live[successor].end());
                const auto& node = original[id];
                if (!node.checkedInitialization) {
                    next.erase(node.write);
                    next.erase(node.reset);
                }
                if (node.destructure)
                    if (const auto* checked =
                            _model.CheckedResultFor(*node.destructure))
                        next.erase(checked->error);
                if (node.condition && node.condition->Kind() ==
                                          core::AstNodeKind::BinaryExpression) {
                    const auto& binary =
                        static_cast<const core::BinaryExpression&>(
                            *node.condition);
                    for (const auto* operand :
                         {&binary.Left(), &binary.Right()}) {
                        const auto& inner = Unwrap(*operand);
                        if (inner.Kind() == core::AstNodeKind::NameExpression) {
                            const auto* symbol = _model.SymbolFor(
                                static_cast<const core::NameExpression&>(
                                    inner));
                            if (errors.contains(symbol)) next.insert(symbol);
                        }
                    }
                }
                if (node.read) {
                    const auto* symbol = _model.SymbolFor(*node.read);
                    if (errors.contains(symbol)) next.insert(symbol);
                }
                if (next != live[id]) {
                    live[id] = std::move(next);
                    changed = true;
                }
            }
        }
        using Key = std::pair<NodeId, Outcomes>;
        std::map<Key, NodeId> ids;
        std::vector<Key> pending;
        auto intern = [&](NodeId id, Outcomes outcomes) {
            std::erase_if(outcomes, [&](const auto& entry) {
                return !live[id].contains(entry.first);
            });
            Key key{id, std::move(outcomes)};
            const auto [it, inserted] = ids.emplace(key, ids.size());
            if (inserted) {
                pending.push_back(key);
                _graph.nodes.push_back(original[id]);
                _graph.nodes.back().successors.clear();
            }
            return it->second;
        };
        // Keep a unique exit even when it is unreachable.
        _graph.exit = intern(_graph.exit, {});
        _graph.entry = intern(_graph.entry, {});
        for (std::size_t cursor = 0; cursor < pending.size(); ++cursor) {
            const auto [old, incoming] = pending[cursor];
            const NodeId id = ids.at(pending[cursor]);
            auto outcomes = incoming;
            const auto& node = original[old];
            if (!node.checkedInitialization) {
                outcomes.erase(node.write);
                outcomes.erase(node.reset);
            }
            if (node.destructure)
                if (const auto* checked =
                        _model.CheckedResultFor(*node.destructure))
                    outcomes[checked->error] = node.asyncSuccess;
            if (node.memoryExpression &&
                node.memoryExpression->Kind() ==
                    core::AstNodeKind::UnaryExpression) {
                const auto& unary = static_cast<const core::UnaryExpression&>(
                    *node.memoryExpression);
                const auto& operand = Unwrap(unary.Operand());
                if (unary.OperatorType() == TokenType::Ampersand &&
                    operand.Kind() == core::AstNodeKind::NameExpression)
                    outcomes.erase(_model.SymbolFor(
                        static_cast<const core::NameExpression&>(operand)));
            }
            if (node.captureRead)
                outcomes.erase(_model.CaptureSource(*node.captureRead));
            std::optional<bool> condition;
            if (node.condition && node.successors.size() == 2) {
                const auto& expression = Unwrap(*node.condition);
                if (expression.Kind() == core::AstNodeKind::BinaryExpression) {
                    const auto& binary =
                        static_cast<const core::BinaryExpression&>(expression);
                    const auto op = binary.OperatorType();
                    if (op == TokenType::Equal || op == TokenType::NotEqual ||
                        op == TokenType::StrictEqual ||
                        op == TokenType::StrictNotEqual) {
                        const auto* name = &Unwrap(binary.Left());
                        const auto* none = &Unwrap(binary.Right());
                        if (_model.TypeOf(*name).Kind() ==
                            SemanticTypeKind::None)
                            std::swap(name, none);
                        if (name->Kind() == core::AstNodeKind::NameExpression &&
                            _model.TypeOf(*none).Kind() ==
                                SemanticTypeKind::None) {
                            const auto* symbol = _model.SymbolFor(
                                static_cast<const core::NameExpression&>(
                                    *name));
                            if (outcomes.contains(symbol))
                                condition = outcomes.at(symbol) ==
                                            (op == TokenType::Equal ||
                                             op == TokenType::StrictEqual);
                        }
                    }
                }
            }
            for (std::size_t edge = 0; edge < node.successors.size(); ++edge) {
                if (condition && edge != (*condition ? 0U : 1U)) continue;
                const NodeId successor =
                    intern(node.successors[edge], outcomes);
                _graph.nodes[id].successors.push_back(successor);
            }
        }
    }

    NodeId BuildBranch(const core::ConditionalBranch& branch, NodeId next,
                       NodeId alternative) {
        const NodeId body = BuildBlock(branch.Body(), next, branch.Also());
        return BuildCondition(branch.Condition(), body, alternative);
    }

    NodeId BuildStatement(const core::Statement& statement, NodeId next) {
        const core::Expression* expression = nullptr;
        switch (statement.Kind()) {
            case core::AstNodeKind::VariableDeclarationStatement:
                expression =
                    static_cast<const core::VariableDeclarationStatement&>(
                        statement)
                        .Initializer();
                break;
            case core::AstNodeKind::DestructureStatement:
                expression =
                    &static_cast<const core::DestructureStatement&>(statement)
                         .Value();
                break;
            case core::AstNodeKind::ReturnStatement:
                expression =
                    static_cast<const core::ReturnStatement&>(statement)
                        .Value();
                break;
            case core::AstNodeKind::ExpressionStatement:
                expression =
                    &static_cast<const core::ExpressionStatement&>(statement)
                         .Value();
                break;
            default:
                break;
        }
        const auto* await = expression && Unwrap(*expression).Kind() ==
                                              core::AstNodeKind::AwaitExpression
                                ? &static_cast<const core::AwaitExpression&>(
                                      Unwrap(*expression))
                                : nullptr;
        const auto* result = await ? _model.AwaitResultFor(*await) : nullptr;
        if (!result) return BuildStatementBody(statement, next);
        const NodeId success = BuildStatementBody(statement, next);
        _graph.nodes[success].awaitOutcome = await;
        NodeId failure;
        const auto failureStart = _graph.nodes.size();
        if (result->forwardsFailure) {
            const NodeId exit = AddNode({});
            _graph.nodes[exit].failureExit = await;
            failure = BuildEvaluation(*expression, exit);
        } else {
            // Void completion constructs only its error binding on either
            // outcome. There is no success-only value and no dummy void slot.
            failure = BuildStatementBody(statement, next);
        }
        _graph.nodes[failure].awaitOutcome = await;
        for (NodeId id = failureStart; id < _graph.nodes.size(); ++id)
            _graph.nodes[id].asyncSuccess = false;
        const NodeId dispatch = AddNode({success, failure});
        _graph.nodes[dispatch].awaitDispatch = await;
        return dispatch;
    }

    NodeId BuildStatementBody(const core::Statement& statement, NodeId next) {
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
                    if (HasTry(*declaration.Initializer())) {
                        const NodeId action = AddNode({store});
                        _graph.nodes[action].declaration = &declaration;
                        return BuildTryAction(*declaration.Initializer(),
                                              action);
                    }
                    entry = BuildExpression(*declaration.Initializer(), store);
                }
                const NodeId action = AddNode({entry});
                _graph.nodes[action].declaration = &declaration;
                return action;
            }
            case core::AstNodeKind::DestructureStatement: {
                const auto& destructure =
                    static_cast<const core::DestructureStatement&>(statement);
                if (_model.CheckedResultFor(destructure)) {
                    const NodeId success =
                        BuildDestructure(destructure, next, true);
                    const NodeId failure =
                        BuildDestructure(destructure, next, false);
                    const NodeId dispatch = AddNode({success, failure});
                    _graph.nodes[dispatch].checkedDispatch = &destructure;
                    return dispatch;
                }
                return BuildDestructure(destructure, next, true);
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
                if (HasTry(deferred.Call())) {
                    const NodeId action = AddNode({registration});
                    _graph.nodes[action].deferRegistration = &deferred;
                    return BuildTryAction(deferred.Call(), action, true);
                }
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
                _graph.nodes[exit].transfer = &statement;
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
            case core::AstNodeKind::BreakStatement: {
                const bool proceed =
                    statement.Kind() == core::AstNodeKind::ProceedStatement;
                const NodeId transfer = AddNode(
                    _loops.empty()
                        ? std::vector<NodeId>{}
                        : std::vector<NodeId>{proceed ? _loops.back().first
                                                      : _loops.back().second});
                _graph.nodes[transfer].transfer = &statement;
                return transfer;
            }
            case core::AstNodeKind::JumpToStatement: {
                const NodeId jump = AddNode({});
                _graph.nodes[jump].transfer = &statement;
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
        if (HasTry(expression)) {
            const NodeId action = AddNode({next});
            _graph.nodes[action].evaluation = &expression;
            _graph.nodes[action].borrowReturn = borrowReturn;
            return BuildTryAction(expression, action);
        }
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
            case core::AstNodeKind::TryExpression: {
                const auto& attempt =
                    static_cast<const core::TryExpression&>(expression);
                const NodeId failure = AddNode({});
                _graph.nodes[failure].abandonedAllocations =
                    _initializingAllocations;
                _graph.nodes[failure].propagationReturn = &attempt;
                _graph.nodes[failure].evaluation = _tryEvaluation;
                const NodeId failureEntry =
                    attempt.PeekBody() == nullptr
                        ? failure
                        : BuildPeekBlock(attempt, failure);
                const NodeId dispatch = AddNode({next, failureEntry});
                _graph.nodes[dispatch].propagationDispatch = &attempt;
                return BuildExpression(attempt.Operand(), dispatch);
            }
            case core::AstNodeKind::AwaitExpression:
                return BuildExpression(
                    static_cast<const core::AwaitExpression&>(expression)
                        .Operand(),
                    next);
            case core::AstNodeKind::SpawnExpression: {
                const auto& spawn =
                    static_cast<const core::SpawnExpression&>(expression);
                if (const Symbol* task = _model.TaskFor(spawn))
                    _graph.symbolScopes.emplace(task, _scopes.back());
                return BuildExpression(spawn.Operand(), next);
            }
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
                if (function && function->BuiltIn() == BuiltInFunction::Crash)
                    next = AddNode({});
                const bool allocation =
                    function &&
                    (function->BuiltIn() == BuiltInFunction::New ||
                     function->BuiltIn() == BuiltInFunction::StorageNew);
                const bool dynamic =
                    allocation && call.TypeArgument() &&
                    call.TypeArgument()->TypeForm() == core::TypeKind::Slice;
                const NodeId failure = next;
                if (dynamic) {
                    next = call.IsCrashable() ? AddNode({next})
                                              : AddNode({next, failure});
                    _graph.nodes[next].allocation = &call;
                }
                const auto arguments = _model.ArgumentsFor(call);
                if (allocation && call.TypeArgument() && !dynamic)
                    _initializingAllocations.push_back(&call);
                for (auto argument = arguments.rbegin();
                     argument != arguments.rend(); ++argument)
                    next = BuildExpression(**argument, next);
                if (allocation && call.TypeArgument() && !dynamic)
                    _initializingAllocations.pop_back();
                if (allocation && call.TypeArgument() && !dynamic) {
                    next = call.IsCrashable() ? AddNode({next})
                                              : AddNode({next, failure});
                    _graph.nodes[next].allocation = &call;
                }
                return BuildExpression(call.Callee(), next);
            }
            case core::AstNodeKind::MemberCallExpression: {
                const auto& call =
                    static_cast<const core::MemberCallExpression&>(expression);
                const auto arguments = _model.ArgumentsFor(call);
                for (auto argument = arguments.rbegin();
                     argument != arguments.rend(); ++argument)
                    next = BuildExpression(**argument, next);
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
    std::vector<const core::CallExpression*> _initializingAllocations;
    const core::Expression* _tryEvaluation = nullptr;
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
