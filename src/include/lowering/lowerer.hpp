#ifndef FLECHA_LOWERING_LOWERER_HPP
#define FLECHA_LOWERING_LOWERER_HPP

#include <memory>
#include <vector>

#include "bound_ir.hpp"
#include "core/ast.hpp"
#include "diagnostics/diagnostic_bag.hpp"
#include "semantic/semantic_model.hpp"

namespace flecha::lowering {

class Lowerer final {
   public:
    [[nodiscard]] std::unique_ptr<BoundProgram> Lower(
        const core::CompilationUnit& unit,
        const semantic::SemanticModel& semanticModel,
        const diagnostics::DiagnosticBag& diagnostics);

   private:
    std::unique_ptr<BoundFunction> LowerFunction(
        const core::FunctionDeclaration& function);
    std::unique_ptr<BoundBlockStatement> LowerBlock(
        const core::BlockStatement& block);
    std::vector<std::unique_ptr<BoundStatement>> LowerStatements(
        const core::BlockStatement& block);
    std::unique_ptr<BoundStatement> LowerStatement(
        const core::Statement& statement);
    std::unique_ptr<BoundStatement> LowerIf(const core::IfStatement& statement);
    std::unique_ptr<BoundStatement> LowerWhile(
        const core::WhileStatement& statement);
    std::unique_ptr<BoundIfStatement> LowerBranch(
        const core::ConditionalBranch& branch,
        std::unique_ptr<BoundBlockStatement> elseBlock);
    std::unique_ptr<BoundPlaceExpression> LowerPlace(
        const core::Expression& expression);
    std::unique_ptr<BoundExpression> LowerExpression(
        const core::Expression& expression,
        semantic::SemanticType expected = semantic::SemanticType::Error());

    const semantic::SemanticModel* _semanticModel = nullptr;
    semantic::SemanticType _returnType = semantic::SemanticType::Error();
    const core::Expression* _directTransfer = nullptr;
    bool _zeroFunction = false;
};

}  // namespace flecha::lowering

#endif  // FLECHA_LOWERING_LOWERER_HPP
