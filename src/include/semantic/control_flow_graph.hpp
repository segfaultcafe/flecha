#ifndef FLECHA_SEMANTIC_CONTROL_FLOW_GRAPH_HPP
#define FLECHA_SEMANTIC_CONTROL_FLOW_GRAPH_HPP

#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "semantic_model.hpp"

namespace flecha::semantic {
using NodeId = std::size_t;
struct FlowNode final {
    std::vector<NodeId> successors;
    std::vector<NodeId> predecessors;
    std::vector<const core::BlockStatement*> scopes;
    // Ordered read/write events used by definite assignment.
    const core::NameExpression* read = nullptr;
    const core::AnonymousFunctionExpression::Capture* captureRead = nullptr;
    const Symbol* write = nullptr;
    const Symbol* reset = nullptr;
    const Symbol* move = nullptr;
    // Whole-expression actions for borrow/move analysis. These precede the
    // corresponding ordered events; each pass consumes only its own events.
    const core::Expression* evaluation = nullptr;
    // Marks an evaluation whose value leaves through a return statement.
    // The borrow checker uses this to validate borrowed-result contracts.
    const core::ReturnStatement* borrowReturn = nullptr;
    const core::VariableDeclarationStatement* declaration = nullptr;
    const core::DestructureStatement* destructure = nullptr;
    const core::DeferStatement* deferRegistration = nullptr;
    // Post-order events for allocation analysis; never consume the borrow
    // actions above in the memory pass (that would evaluate effects twice).
    const core::Expression* memoryExpression = nullptr;
    const core::Expression* memoryUntrackedStore = nullptr;
    const core::Expression* memoryDiscard = nullptr;
    const core::Expression* memoryBoolean = nullptr;
    bool booleanValue = false;
    const core::Expression* condition = nullptr;
    const core::CallExpression* allocation = nullptr;
    const core::VariableDeclarationStatement* memoryDeclaration = nullptr;
    const core::DestructureStatement* memoryDestructure = nullptr;
    const core::DeferStatement* memoryDefer = nullptr;
    const core::ReturnStatement* memoryReturn = nullptr;
    bool reachable = false;
};
struct ControlFlowGraph final {
    std::vector<FlowNode> nodes;
    std::unordered_map<const Symbol*, const core::BlockStatement*> symbolScopes;
    std::unordered_map<const core::BlockStatement*, const core::BlockStatement*>
        scopeParents;
    std::unordered_set<const Symbol*> deferredCaptures;
    NodeId entry = 0;
    NodeId exit = 0;
};
[[nodiscard]] ControlFlowGraph BuildControlFlowGraph(
    const core::FunctionDeclaration& function, const SemanticModel& model);
[[nodiscard]] bool CanReachFunctionExit(const ControlFlowGraph& graph);
}  // namespace flecha::semantic
#endif
