#include "semantic/definite_assignment.hpp"

#include <algorithm>
#include <deque>
#include <unordered_set>

#include "semantic/control_flow_graph.hpp"
namespace flecha::semantic {
void AnalyzeDefiniteAssignment(const core::FunctionDeclaration& function,
                               const SemanticModel& model,
                               diagnostics::DiagnosticBag& diagnostics,
                               const ControlFlowGraph& graph) {
    using SymbolSet = std::unordered_set<const Symbol*>;
    SymbolSet parameters;
    for (const auto& parameter : function.Parameters()) {
        const Symbol* symbol = model.DeclaredSymbol(*parameter);
        if (symbol != nullptr) parameters.insert(symbol);
    }
    for (const ClosureCapture& capture : model.Captures(function))
        parameters.insert(capture.variable);
    const NodeId entry = graph.entry;
    std::deque<NodeId> worklist;
    for (NodeId index = 0; index < graph.nodes.size(); ++index) {
        if (graph.nodes[index].reachable) worklist.push_back(index);
    }
    // Must analysis starts at top, then descends to the greatest fixed
    // point. Only entry has a boundary state; unreachable cycles contribute
    // no predecessors. A backedge cannot initialize the first iteration.
    SymbolSet universe = parameters;
    for (const auto& [symbol, scope] : graph.symbolScopes) {
        universe.insert(symbol);
    }
    std::vector<SymbolSet> outgoing(graph.nodes.size(), universe);
    std::vector<SymbolSet> incomingStates(graph.nodes.size());
    std::vector<SymbolSet> movedOutgoing(graph.nodes.size());
    std::vector<SymbolSet> movedIncoming(graph.nodes.size());
    std::vector<bool> queued(graph.nodes.size(), true);
    while (!worklist.empty()) {
        const NodeId index = worklist.front();
        worklist.pop_front();
        queued[index] = false;
        const FlowNode& node = graph.nodes[index];
        SymbolSet incoming = index == entry ? parameters : universe;
        SymbolSet moved;
        for (NodeId predecessor : node.predecessors) {
            if (!graph.nodes[predecessor].reachable) continue;
            std::erase_if(incoming, [&](const Symbol* symbol) {
                return !outgoing[predecessor].contains(symbol);
            });
            moved.insert(movedOutgoing[predecessor].begin(),
                         movedOutgoing[predecessor].end());
        }
        // Leaving a lexical scope ends its locals' lifetimes. This also
        // prevents a later jump back into that scope from reusing
        // initialization from a previous activation.
        std::erase_if(incoming, [&](const Symbol* symbol) {
            const auto scope = graph.symbolScopes.find(symbol);
            return scope != graph.symbolScopes.end() &&
                   std::find(node.scopes.begin(), node.scopes.end(),
                             scope->second) == node.scopes.end();
        });
        std::erase_if(moved, [&](const Symbol* symbol) {
            const auto scope = graph.symbolScopes.find(symbol);
            return scope != graph.symbolScopes.end() &&
                   std::find(node.scopes.begin(), node.scopes.end(),
                             scope->second) == node.scopes.end();
        });
        incomingStates[index] = incoming;
        movedIncoming[index] = moved;
        if (node.reset != nullptr) {
            incoming.erase(node.reset);
            moved.erase(node.reset);
        }
        if (node.move != nullptr) {
            incoming.erase(node.move);
            moved.insert(node.move);
        }
        if (node.write != nullptr) {
            incoming.insert(node.write);
            moved.erase(node.write);
        }
        if (incoming == outgoing[index] && moved == movedOutgoing[index])
            continue;
        outgoing[index] = std::move(incoming);
        movedOutgoing[index] = std::move(moved);
        for (NodeId successor : node.successors) {
            if (!queued[successor]) {
                queued[successor] = true;
                worklist.push_back(successor);
            }
        }
    }

    // Source order is stable even though the graph is built backwards.
    struct InvalidRead final {
        const core::NameExpression* expression;
        bool moved;
    };
    std::vector<InvalidRead> uninitialized;
    for (NodeId index = 0; index < graph.nodes.size(); ++index) {
        const FlowNode& node = graph.nodes[index];
        if (!node.reachable ||
            (node.read == nullptr && node.captureRead == nullptr))
            continue;
        const Symbol* symbol = node.read != nullptr
                                   ? model.SymbolFor(*node.read)
                                   : model.CaptureSource(*node.captureRead);
        if (symbol != nullptr && symbol->Kind() != SymbolKind::Function &&
            symbol->Kind() != SymbolKind::ModuleConstant &&
            !incomingStates[index].contains(symbol)) {
            if (node.read != nullptr) {
                uninitialized.push_back(
                    {node.read, movedIncoming[index].contains(symbol)});
            } else {
                diagnostics.Report(
                    movedIncoming[index].contains(symbol)
                        ? diagnostics::DiagnosticCode::MovedValue
                        : diagnostics::DiagnosticCode::UninitializedVariable,
                    node.captureRead->NameRange(),
                    "variable '" + node.captureRead->Name() +
                        (movedIncoming[index].contains(symbol)
                             ? "' may have been moved"
                             : "' may be read before it is initialized"));
            }
        }
    }
    std::sort(uninitialized.begin(), uninitialized.end(),
              [](const InvalidRead& left, const InvalidRead& right) {
                  return left.expression->Range().start.offset <
                         right.expression->Range().start.offset;
              });
    for (const InvalidRead& invalid : uninitialized) {
        const auto code =
            invalid.moved ? diagnostics::DiagnosticCode::MovedValue
                          : diagnostics::DiagnosticCode::UninitializedVariable;
        diagnostics.Report(
            code, invalid.expression->Range(),
            "variable '" + invalid.expression->Name() +
                (invalid.moved ? "' may have been moved"
                               : "' may be read before it is initialized"));
    }
}
}  // namespace flecha::semantic
