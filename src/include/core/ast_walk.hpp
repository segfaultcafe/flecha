#pragma once

#include <functional>

#include "core/ast.hpp"

namespace flecha::core {
// One structural walk is shared by rewriting, substitution-site discovery, and
// template checks. Re-parsing templates preserves all existing desugarings.
inline void WalkAst(const AstNode& input,
                    const std::function<void(AstNode&, bool)>& callback) {
    auto& node = const_cast<AstNode&>(input);
    callback(node, true);
    auto visit = [&](const AstNode& child) { WalkAst(child, callback); };
    auto optional = [&](const auto* child) {
        if (child) visit(*child);
    };
    auto list = [&](const auto& children) {
        for (const auto& child : children) visit(*child);
    };
#define NODE(Type) const auto& n = static_cast<const Type&>(node)
    switch (node.Kind()) {
        case AstNodeKind::Type: {
            NODE(TypeNode);
            optional(n.Target());
            list(n.Elements());
            list(n.typeArguments);
            break;
        }
        case AstNodeKind::FunctionDeclaration: {
            NODE(FunctionDeclaration);
            visit(n.ReturnType());
            list(n.Parameters());
            visit(n.Body());
            break;
        }
        case AstNodeKind::AnonymousFunctionExpression: {
            NODE(AnonymousFunctionExpression);
            visit(n.Function());
            break;
        }
        case AstNodeKind::CallableTypeDeclaration: {
            NODE(CallableTypeDeclaration);
            list(n.Parameters());
            list(n.Results());
            break;
        }
        case AstNodeKind::StructureDeclaration: {
            NODE(StructureDeclaration);
            list(n.Fields());
            break;
        }
        case AstNodeKind::EnumDeclaration: {
            NODE(EnumDeclaration);
            optional(n.BackingType());
            break;
        }
        case AstNodeKind::BehaviorDeclaration: {
            NODE(BehaviorDeclaration);
            // Only closed signature types participate in normal generic
            // expansion. Bodies are staged separately by detached behavior planning.
            const auto dependent = [&](const auto& self,
                                       const TypeNode& type) -> bool {
                if (type.TypeForm() == TypeKind::Named &&
                    type.Name() == n.targetParameter)
                    return true;
                if (type.Target() && self(self, *type.Target())) return true;
                for (const auto& element : type.Elements())
                    if (self(self, *element)) return true;
                for (const auto& argument : type.typeArguments)
                    if (self(self, *argument)) return true;
                return false;
            };
            for (const auto& method : n.methods) {
                if (!dependent(dependent, method->ReturnType()))
                    visit(method->ReturnType());
                for (const auto& parameter : method->Parameters())
                    if (parameter->Name() != "self" &&
                        !dependent(dependent, parameter->Type()))
                        visit(parameter->Type());
            }
            break;
        }
        case AstNodeKind::InterfaceDeclaration: {
            NODE(InterfaceDeclaration);
            list(n.Methods());
            break;
        }
        case AstNodeKind::InterfaceMethod: {
            NODE(InterfaceMethod);
            visit(n.Signature());
            break;
        }
        case AstNodeKind::TypeSetDeclaration: {
            NODE(TypeSetDeclaration);
            list(n.Members());
            break;
        }
        case AstNodeKind::ModuleConstantDeclaration: {
            NODE(ModuleConstantDeclaration);
            visit(n.Type());
            visit(n.Initializer());
            break;
        }
        case AstNodeKind::StructureField: {
            NODE(StructureField);
            visit(n.Type());
            optional(n.Initializer());
            break;
        }
        case AstNodeKind::Parameter: {
            NODE(Parameter);
            visit(n.Type());
            optional(n.DefaultValue());
            break;
        }
        case AstNodeKind::BlockStatement: {
            NODE(BlockStatement);
            list(n.Statements());
            break;
        }
        case AstNodeKind::ReturnStatement: {
            NODE(ReturnStatement);
            optional(n.Value());
            break;
        }
        case AstNodeKind::AlsoClause: {
            NODE(AlsoClause);
            visit(n.Condition());
            visit(n.Body());
            break;
        }
        case AstNodeKind::ConditionalBranch: {
            NODE(ConditionalBranch);
            visit(n.Condition());
            visit(n.Body());
            optional(n.Also());
            break;
        }
        case AstNodeKind::IfStatement: {
            NODE(IfStatement);
            visit(n.PrimaryBranch());
            list(n.ElseIfBranches());
            optional(n.ElseBody());
            break;
        }
        case AstNodeKind::WhileStatement: {
            NODE(WhileStatement);
            visit(n.Condition());
            visit(n.Body());
            optional(n.Also());
            optional(n.Update());
            break;
        }
        case AstNodeKind::FieldIterationStatement: {
            NODE(FieldIterationStatement);
            visit(*n.pack);
            visit(*n.body);
            break;
        }
        case AstNodeKind::RangeStatement: {
            NODE(RangeStatement);
            visit(n.Expansion());
            break;
        }
        case AstNodeKind::VariableDeclarationStatement: {
            NODE(VariableDeclarationStatement);
            optional(n.ExplicitType());
            optional(n.Initializer());
            break;
        }
        case AstNodeKind::DestructureStatement: {
            NODE(DestructureStatement);
            list(n.Bindings());
            visit(n.Value());
            break;
        }
        case AstNodeKind::DestructureBinding: {
            NODE(DestructureBinding);
            optional(n.ExplicitType());
            break;
        }
        case AstNodeKind::DeferStatement: {
            NODE(DeferStatement);
            visit(n.Call());
            break;
        }
        case AstNodeKind::ExpressionStatement: {
            NODE(ExpressionStatement);
            visit(n.Value());
            break;
        }
        case AstNodeKind::TupleExpression: {
            NODE(TupleExpression);
            list(n.Elements());
            break;
        }
        case AstNodeKind::ArrayExpression: {
            NODE(ArrayExpression);
            list(n.Elements());
            break;
        }
        case AstNodeKind::MatchExpression: {
            NODE(MatchExpression);
            visit(n.Value());
            list(n.Arms());
            break;
        }
        case AstNodeKind::MatchArm: {
            NODE(MatchArm);
            visit(n.Value());
            break;
        }
        case AstNodeKind::NameExpression: {
            NODE(NameExpression);
            list(n.typeArguments);
            break;
        }
        case AstNodeKind::ParenthesizedExpression: {
            NODE(ParenthesizedExpression);
            visit(n.Inner());
            break;
        }
        case AstNodeKind::CastExpression: {
            NODE(CastExpression);
            visit(n.Value());
            visit(n.TargetType());
            break;
        }
        case AstNodeKind::IsOfExpression: {
            NODE(IsOfExpression);
            visit(n.Value());
            visit(n.Target());
            break;
        }
        case AstNodeKind::UnaryExpression: {
            NODE(UnaryExpression);
            visit(n.Operand());
            break;
        }
        case AstNodeKind::BinaryExpression: {
            NODE(BinaryExpression);
            visit(n.Left());
            visit(n.Right());
            break;
        }
        case AstNodeKind::AssignmentExpression: {
            NODE(AssignmentExpression);
            visit(n.Target());
            visit(n.Value());
            break;
        }
        case AstNodeKind::CallExpression: {
            NODE(CallExpression);
            visit(n.Callee());
            optional(n.TypeArgument());
            list(n.Arguments());
            break;
        }
        case AstNodeKind::MemberCallExpression: {
            NODE(MemberCallExpression);
            visit(n.Receiver());
            list(n.typeArguments);
            list(n.Arguments());
            break;
        }
        case AstNodeKind::MemberAccessExpression: {
            NODE(MemberAccessExpression);
            visit(n.Receiver());
            break;
        }
        case AstNodeKind::SliceExpression: {
            NODE(SliceExpression);
            visit(n.Receiver());
            optional(n.Start());
            optional(n.End());
            break;
        }
        case AstNodeKind::IndexExpression: {
            NODE(IndexExpression);
            visit(n.Receiver());
            visit(n.Index());
            break;
        }
        case AstNodeKind::BuildExpression: {
            NODE(BuildExpression);
            visit(n.Type());
            visit(n.Body());
            break;
        }
        case AstNodeKind::CompilationUnit:
        case AstNodeKind::AnonymousCapture:
        case AstNodeKind::ErrorDeclaration:
        case AstNodeKind::DecorationDeclaration:
        case AstNodeKind::ProceedStatement:
        case AstNodeKind::BreakStatement:
        case AstNodeKind::JumpToStatement:
        case AstNodeKind::PositionStatement:
        case AstNodeKind::LiteralExpression:
            break;
    }
#undef NODE
    callback(node, false);
}
}  // namespace flecha::core
