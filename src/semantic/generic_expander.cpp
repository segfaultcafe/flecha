#include "semantic/generic_expander.hpp"

#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <sstream>

#include "core/parser.hpp"
#include "core/tokenizer.hpp"
#include "semantic/semantic_type.hpp"

namespace flecha::semantic {
namespace {
using namespace core;
using diagnostics::DiagnosticCode;
using Tokens = std::vector<Token>;
using Arguments = std::vector<Tokens>;
using Callback = std::function<void(AstNode&, bool)>;

bool IsPrimitiveNumericTypeName(const std::string& name) {
    return name == "i8" || name == "i16" || name == "i32" ||
           name == "i64" || name == "isize" || name == "u8" ||
           name == "u16" || name == "u32" || name == "u64" ||
           name == "usize" || name == "f32" || name == "f64";
}

// One structural walk is shared by rewriting, substitution-site discovery, and
// template checks. Re-parsing templates preserves all existing desugarings.
void Walk(const AstNode& input, const Callback& callback) {
    auto& node = const_cast<AstNode&>(input);
    callback(node, true);
    auto visit = [&](const AstNode& child) { Walk(child, callback); };
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
        case AstNodeKind::TypeSetDeclaration: {
            NODE(TypeSetDeclaration);
            list(n.Members());
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
std::string Name(const Declaration& declaration) {
    if (declaration.Kind() == AstNodeKind::FunctionDeclaration)
        return static_cast<const FunctionDeclaration&>(declaration).Name();
    if (declaration.Kind() == AstNodeKind::StructureDeclaration)
        return static_cast<const StructureDeclaration&>(declaration).Name();
    if (declaration.Kind() == AstNodeKind::EnumDeclaration)
        return static_cast<const EnumDeclaration&>(declaration).Name();
    if (declaration.Kind() == AstNodeKind::CallableTypeDeclaration)
        return static_cast<const CallableTypeDeclaration&>(declaration).Name();
    if (declaration.Kind() == AstNodeKind::TypeSetDeclaration)
        return static_cast<const TypeSetDeclaration&>(declaration).Name();
    return static_cast<const ErrorDeclaration&>(declaration).Name();
}
bool Template(const Declaration& d) {
    return !d.genericParameters.empty() || !d.ownerParameters.empty();
}
std::string Key(const Arguments& arguments) {
    std::string key;
    for (const auto& argument : arguments) {
        key += "[";
        for (const auto& token : argument)
            key += std::to_string(static_cast<int>(token.type)) + ":" +
                   token.value + ";";
        key += "]";
    }
    return key;
}
struct Instance {
    const Declaration* origin;
    Declaration* concrete;
    Arguments arguments;
};
struct MethodRequest {
    std::string name, concreteName;
    Arguments arguments;
    source::SourceRange range;
};
class Expander {
   public:
    Expander(CompilationUnit& unit, diagnostics::DiagnosticBag& diagnostics)
        : unit(unit), diagnostics(diagnostics) {}
    void Run() {
        if (unit.genericsExpanded) return;
        unit.genericsExpanded = true;
        for (const auto& module : unit.modules)
            modules[module.sourceId] = &module;
        auto originals = unit.TakeDeclarations();
        for (const auto& d : originals) {
            if (d->Kind() == AstNodeKind::FunctionDeclaration &&
                !static_cast<const FunctionDeclaration&>(*d).Owner().empty()) {
                methods.push_back(
                    static_cast<const FunctionDeclaration*>(d.get()));
                continue;
            }
            const auto [existing, inserted] = declarations.emplace(
                std::make_pair(d->moduleId, Name(*d)), d.get());
            if (!inserted &&
                (Template(*d) || Template(*existing->second) ||
                 d->Kind() == AstNodeKind::TypeSetDeclaration ||
                 existing->second->Kind() == AstNodeKind::TypeSetDeclaration))
                diagnostics.Report(DiagnosticCode::DuplicateDeclaration,
                                   d->Range(),
                                   "duplicate declaration '" + Name(*d) + "'");
            if (modules.contains(d->sourceId))
                for (const auto& import : modules[d->sourceId]->imports)
                    if (Template(*d) && import.alias == Name(*d))
                        Error(d->Range(),
                              "import alias conflicts with declaration '" +
                                  import.alias + "'");
        }
        {
            std::set<const Declaration*> visiting, validated;
            for (const auto& [key, declaration] : declarations) {
                (void)key;
                if (declaration->Kind() == AstNodeKind::TypeSetDeclaration)
                    ValidateTypeSet(
                        static_cast<const TypeSetDeclaration&>(*declaration),
                        visiting, validated);
            }
        }
        std::map<std::pair<const Declaration*, std::string>,
                 const FunctionDeclaration*>
            methodNames;
        for (const auto* method : methods) {
            const auto found =
                declarations.find({method->moduleId, method->Owner()});
            if (found == declarations.end() ||
                (found->second->Kind() != AstNodeKind::StructureDeclaration &&
                 found->second->Kind() != AstNodeKind::EnumDeclaration)) {
                if (Template(*method))
                    Error(method->Range(),
                          "unknown method owner '" + method->Owner() + "'");
                continue;
            }
            const auto* owner = found->second;
            const auto [previous, inserted] = methodNames.emplace(
                std::make_pair(owner, method->Name()), method);
            if (!inserted && (Template(*method) || Template(*previous->second)))
                Error(method->Range(),
                      "duplicate method declaration '" + method->Name() + "'");
            if (Template(*method) &&
                owner->Kind() == AstNodeKind::EnumDeclaration)
                for (const auto& variant :
                     static_cast<const EnumDeclaration*>(owner)->Variants())
                    if (variant.name == method->Name())
                        Error(method->Range(),
                              "generic method conflicts with enum variant '" +
                                  method->Name() + "'");
            if (method->ownerParameters != owner->genericParameters)
                Error(method->Range(),
                      "method owner must repeat its owning type's type "
                      "parameter names");
            std::set<std::string> parameters(method->ownerParameters.begin(),
                                             method->ownerParameters.end());
            for (const auto& p : method->genericParameters)
                if (!parameters.insert(p).second)
                    Error(method->Range(),
                          "method type parameter shadows its owner's type "
                          "parameter '" +
                              p + "'");
            if (!method->genericParameters.empty() &&
                (method->Name() == "init" || method->Name() == "tear"))
                Error(method->Range(),
                      "lifecycle methods cannot declare additional type "
                      "parameters");
            if (!owner->genericParameters.empty() ||
                !method->genericParameters.empty())
                genericMethods[owner].push_back(method);
            else
                concreteMethods[{owner, method->Name()}] = method;
        }
        for (auto& declaration : originals) {
            if (declaration->Kind() == AstNodeKind::FunctionDeclaration &&
                static_cast<const FunctionDeclaration&>(*declaration)
                    .IsExtern() &&
                Template(*declaration))
                diagnostics.Report(
                    DiagnosticCode::InvalidExtern, declaration->Range(),
                    "extern declarations cannot have generic parameters");
            Validate(*declaration);
            if (declaration->Kind() == AstNodeKind::TypeSetDeclaration)
                unit.typeSets.push_back(std::move(declaration));
            else if (Template(*declaration))
                unit.genericTemplates.push_back(std::move(declaration));
            else
                unit.AddDeclaration(std::move(declaration));
        }
        // Non-generic owners can have generic methods as well.
        for (const auto& [owner, ownerMethods] : genericMethods)
            if (owner->genericParameters.empty())
                instances.push_back(
                    {owner, const_cast<Declaration*>(owner), {}});
        for (std::size_t index = 0;
             index < unit.Declarations().size() && !failedBudget; ++index) {
            current = unit.Declarations()[index].get();
            locals.clear();
            locals.emplace_back();
            localTypes.clear();
            localTypes.emplace_back();
            Walk(*current,
                 [&](AstNode& node, bool enter) { Rewrite(node, enter); });
        }
    }

   private:
    void Error(source::SourceRange range, const std::string& message) {
        diagnostics.Report(DiagnosticCode::InvalidGeneric, range, message);
    }
    static bool BuiltinTypeSet(const std::string& name) {
        return name == "signed_integer" || name == "unsigned_integer" ||
               name == "integer" || name == "floating_point" ||
               name == "numeric";
    }
    static bool PrimitiveMember(const Tokens& argument,
                                const std::string& set) {
        std::size_t index = 0;
        if (index < argument.size() && argument[index].type == TokenType::Const)
            ++index;
        if (index + 1 != argument.size()) return false;
        const TokenType type = argument[index].type;
        const bool signedInteger =
            type == TokenType::I8 || type == TokenType::I16 ||
            type == TokenType::I32 || type == TokenType::I64 ||
            type == TokenType::ISize;
        const bool unsignedInteger =
            type == TokenType::U8 || type == TokenType::U16 ||
            type == TokenType::U32 || type == TokenType::U64 ||
            type == TokenType::USize;
        const bool floatingPoint =
            type == TokenType::F32 || type == TokenType::F64;
        if (set == "signed_integer") return signedInteger;
        if (set == "unsigned_integer") return unsignedInteger;
        if (set == "integer") return signedInteger || unsignedInteger;
        if (set == "floating_point") return floatingPoint;
        return set == "numeric" &&
               (signedInteger || unsignedInteger || floatingPoint);
    }
    static Tokens Unqualified(Tokens tokens) {
        if (!tokens.empty() && tokens.front().type == TokenType::Const)
            tokens.erase(tokens.begin());
        return tokens;
    }
    const Declaration* LookupFrom(const std::string& name,
                                  const Declaration& context,
                                  source::SourceRange range,
                                  bool diagnose = true) {
        Declaration* saved = current;
        current = const_cast<Declaration*>(&context);
        const Declaration* result =
            Lookup(name, context.moduleId, range, diagnose);
        current = saved;
        return result;
    }
    bool Satisfies(const Tokens& argument, const std::string& setName,
                   const Declaration& context, source::SourceRange range,
                   std::set<const Declaration*>& visiting) {
        if (BuiltinTypeSet(setName)) return PrimitiveMember(argument, setName);
        const Declaration* declaration =
            LookupFrom(setName, context, range, true);
        if (!declaration) {
            Error(range, "unknown type set '" + setName + "'");
            return false;
        }
        if (declaration->Kind() != AstNodeKind::TypeSetDeclaration) {
            Error(range, "'" + setName + "' is not a type set");
            return false;
        }
        if (!visiting.insert(declaration).second) {
            Error(range, "cyclic type-set definition involving '" +
                             Name(*declaration) + "'");
            return false;
        }

        const auto& set = static_cast<const TypeSetDeclaration&>(*declaration);
        for (const auto& member : set.Members()) {
            if (member->TypeForm() == TypeKind::Named &&
                member->typeArguments.empty()) {
                const std::string& nestedName = member->Name();
                if (BuiltinTypeSet(nestedName)) {
                    if (PrimitiveMember(argument, nestedName)) {
                        visiting.erase(declaration);
                        return true;
                    }
                    continue;
                }
                const Declaration* nested = LookupFrom(nestedName, *declaration,
                                                       member->Range(), false);
                if (nested &&
                    nested->Kind() == AstNodeKind::TypeSetDeclaration) {
                    if (Satisfies(argument, nestedName, *declaration,
                                  member->Range(), visiting)) {
                        visiting.erase(declaration);
                        return true;
                    }
                    continue;
                }
            }

            Declaration* saved = current;
            current = const_cast<Declaration*>(declaration);
            Tokens concrete = TypeTokens(const_cast<TypeNode&>(*member),
                                         declaration->moduleId);
            current = saved;
            if (Key({Unqualified(concrete)}) == Key({Unqualified(argument)})) {
                visiting.erase(declaration);
                return true;
            }
        }
        visiting.erase(declaration);
        return false;
    }
    bool Satisfies(const Tokens& argument, const std::string& setName,
                   const Declaration& context, source::SourceRange range) {
        std::set<const Declaration*> visiting;
        return Satisfies(argument, setName, context, range, visiting);
    }
    bool ValidateConstraints(const Declaration& origin,
                             const Arguments& arguments,
                             source::SourceRange range) {
        bool valid = true;
        const std::size_t offset = origin.ownerParameters.size();
        for (std::size_t index = 0; index < origin.genericParameters.size();
             ++index) {
            const auto found =
                origin.genericConstraints.find(origin.genericParameters[index]);
            if (found == origin.genericConstraints.end()) continue;
            if (offset + index >= arguments.size() ||
                !Satisfies(arguments[offset + index], found->second, origin,
                           range)) {
                Error(range, "type argument for '" +
                                 origin.genericParameters[index] +
                                 "' does not satisfy type set '" +
                                 found->second + "'");
                valid = false;
            }
        }
        return valid;
    }
    bool VisibleTypeSetMember(const Declaration& member,
                              const TypeSetDeclaration& set) const {
        if (set.exported) return member.exported;
        if (set.quiverVisible)
            return member.exported ||
                   (member.quiverVisible && member.quiverId == set.quiverId);
        return true;
    }
    void ValidateTypeSet(const TypeSetDeclaration& set,
                         std::set<const Declaration*>& visiting,
                         std::set<const Declaration*>& validated) {
        if (validated.contains(&set)) return;
        if (BuiltinTypeSet(set.Name())) {
            Error(set.NameRange(),
                  "cannot redeclare built-in type set '" + set.Name() + "'");
            validated.insert(&set);
            return;
        }
        if (!visiting.insert(&set).second) {
            Error(set.Range(),
                  "cyclic type-set definition involving '" + set.Name() + "'");
            return;
        }
        for (const auto& member : set.Members()) {
            const Declaration* declaration = nullptr;
            if (member->TypeForm() == TypeKind::Named &&
                member->typeArguments.empty() && BuiltinTypeSet(member->Name()))
                continue;
            if (member->TypeForm() == TypeKind::Named &&
                member->typeArguments.empty() &&
                !BuiltinTypeSet(member->Name())) {
                declaration =
                    LookupFrom(member->Name(), set, member->Range(), false);
                if (declaration &&
                    declaration->Kind() == AstNodeKind::TypeSetDeclaration) {
                    if (!VisibleTypeSetMember(*declaration, set))
                        Error(member->Range(),
                              "visible type sets cannot include less-visible "
                              "type sets");
                    ValidateTypeSet(
                        static_cast<const TypeSetDeclaration&>(*declaration),
                        visiting, validated);
                    continue;
                }
            }

            Declaration* saved = current;
            current = const_cast<TypeSetDeclaration*>(&set);
            Tokens tokens =
                TypeTokens(const_cast<TypeNode&>(*member), set.moduleId);
            current = saved;
            (void)tokens;
            if (!declaration && member->TypeForm() == TypeKind::Named)
                declaration =
                    LookupFrom(member->Name(), set, member->Range(), false);
            if (declaration && !VisibleTypeSetMember(*declaration, set))
                Error(member->Range(),
                      "visible type sets cannot include less-visible types");
        }
        visiting.erase(&set);
        validated.insert(&set);
    }
    bool Local(const std::string& name) const {
        for (const auto& scope : locals)
            if (scope.contains(name)) return true;
        return false;
    }
    bool CanImport(const Declaration& declaration, const Module& from,
                   const std::string& target) const {
        return declaration.exported ||
               (declaration.quiverVisible &&
                declaration.quiverId == from.quiverId) ||
               (from.test && from.testPair == target);
    }
    std::string QuiverName(const Declaration& declaration) const {
        for (const auto& item : modules)
            if (item.second->quiverId == declaration.quiverId)
                return item.second->quiverName;
        return declaration.quiverId;
    }
    const Declaration* Lookup(const std::string& name,
                              const std::string& module,
                              source::SourceRange range, bool diagnose = true) {
        if (const auto alias = unit.genericAliases.find(name);
            alias != unit.genericAliases.end())
            return alias->second;
        std::string target = module, shortName = name;
        const auto separator = name.find("::");
        const Module* source = current && modules.contains(current->sourceId)
                                   ? modules.at(current->sourceId)
                                   : nullptr;
        if (separator != std::string::npos) {
            const auto prefix = name.substr(0, separator);
            shortName = name.substr(separator + 2);
            if (!source) return nullptr;
            std::vector<const Declaration*> accessible;
            const Declaration* inaccessible = nullptr;
            for (const auto& import : source->imports)
                if (import.alias == prefix) {
                    target = import.resolvedModule;
                    const auto found = declarations.find({target, shortName});
                    if (found == declarations.end()) continue;
                    if (!CanImport(*found->second, *source, target)) {
                        inaccessible = found->second;
                        continue;
                    }
                    if (std::find(accessible.begin(), accessible.end(),
                                  found->second) == accessible.end())
                        accessible.push_back(found->second);
                }
            if (accessible.size() > 1) {
                if (diagnose)
                    Error(range, "ambiguous imported generic name '" + name +
                                     "'; use explicit import aliases");
                return nullptr;
            }
            if (accessible.size() == 1) return accessible.front();
            if (inaccessible && diagnose)
                Error(range, inaccessible->quiverVisible
                                 ? "'" + name +
                                       "' is quiver-visible and can only be "
                                       "accessed from modules in '" +
                                       QuiverName(*inaccessible) + "'"
                                 : "generic argument or declaration '" + name +
                                       "' is module-private");
            return nullptr;
        }
        const auto result = declarations.find({target, shortName});
        if (result == declarations.end()) return nullptr;
        return result->second;
    }
    std::string Alias(const Declaration& declaration) {
        if (const auto found = aliases.find(&declaration);
            found != aliases.end())
            return found->second;
        const std::string alias =
            "$generic_alias" + std::to_string(aliases.size());
        aliases[&declaration] = alias;
        unit.genericAliases[alias] = &declaration;
        return alias;
    }
    Tokens TypeTokens(TypeNode& type, const std::string& module) {
        Tokens result;
        const auto emit = [&](TokenType kind, const std::string& spelling) {
            result.emplace_back(kind, spelling, type.Range());
        };
        if (type.IsConst()) emit(TokenType::Const, "const");
        if (type.TypeForm() == TypeKind::Named) {
            const Declaration* declaration =
                Lookup(type.Name(), module, type.Range());
            if (!type.typeArguments.empty()) {
                auto arguments = ResolveArguments(type.typeArguments, module);
                if (!declaration ||
                    (declaration->Kind() != AstNodeKind::StructureDeclaration &&
                     declaration->Kind() !=
                         AstNodeKind::CallableTypeDeclaration)) {
                    Error(type.Range(),
                          "unknown generic type '" + type.Name() + "'");
                    return result;
                }
                declaration =
                    Instantiate(*declaration, arguments, type.Range());
            } else if (declaration && !declaration->genericParameters.empty()) {
                Error(type.Range(), "generic type '" + type.Name() +
                                        "' requires explicit type arguments");
                return result;
            }
            if (!declaration ||
                declaration->Kind() == AstNodeKind::FunctionDeclaration) {
                Error(type.Range(),
                      "unknown concrete type '" + type.Name() + "'");
                return result;
            }
            const auto alias = Alias(*declaration);
            type.Rename(alias);
            type.typeArguments.clear();
            emit(TokenType::Identifier, alias);
        } else if (type.TypeForm() == TypeKind::Primitive) {
            diagnostics::DiagnosticBag ignored;
            auto tokens =
                Tokenizer(SemanticType::FromSyntax(type.Type()).Name(), ignored)
                    .Tokenize();
            result.emplace_back(tokens[0].type, tokens[0].value, type.Range());
        } else if (type.TypeForm() == TypeKind::Tuple) {
            emit(TokenType::LParen, "(");
            for (std::size_t i = 0; i < type.Elements().size(); ++i) {
                if (i) emit(TokenType::Comma, ",");
                auto child = TypeTokens(
                    const_cast<TypeNode&>(*type.Elements()[i]), module);
                result.insert(result.end(), child.begin(), child.end());
            }
            emit(TokenType::RParen, ")");
        } else {
            if (type.TypeForm() == TypeKind::Pointer) emit(TokenType::Mul, "*");
            if (type.TypeForm() == TypeKind::Reference)
                emit(TokenType::Ampersand, "&");
            if (type.TypeForm() == TypeKind::Array ||
                type.TypeForm() == TypeKind::Slice) {
                emit(TokenType::LBracket, "[");
                if (type.TypeForm() == TypeKind::Array)
                    emit(TokenType::NumberLiteral,
                         std::to_string(type.Length()));
                emit(TokenType::RBracket, "]");
            }
            auto child =
                TypeTokens(const_cast<TypeNode&>(*type.Target()), module);
            result.insert(result.end(), child.begin(), child.end());
        }
        return result;
    }
    Arguments ResolveArguments(std::vector<std::unique_ptr<TypeNode>>& syntax,
                               const std::string& module) {
        Arguments result;
        for (auto& type : syntax) result.push_back(TypeTokens(*type, module));
        return result;
    }
    void ValidateApiType(const TypeNode& type, const Declaration& api,
                         const std::set<std::string>& parameters) {
        if (type.TypeForm() == TypeKind::Named &&
            !parameters.contains(type.Name())) {
            const auto* named = Lookup(type.Name(), api.moduleId, type.Range());
            const bool visible =
                !named || (api.exported && named->exported) ||
                (api.quiverVisible &&
                 (named->exported ||
                  (named->quiverVisible && named->quiverId == api.quiverId)));
            if (!visible)
                Error(type.Range(), api.exported
                                        ? "exported APIs cannot expose "
                                          "non-exported types"
                                        : "quiver APIs cannot expose "
                                          "module-private types");
        }
        if (type.Target()) ValidateApiType(*type.Target(), api, parameters);
        for (const auto& element : type.Elements())
            ValidateApiType(*element, api, parameters);
        for (const auto& argument : type.typeArguments)
            ValidateApiType(*argument, api, parameters);
    }
    void Validate(const Declaration& declaration) {
        if (!Template(declaration)) return;
        const Declaration* constraintApi = &declaration;
        if (declaration.Kind() == AstNodeKind::FunctionDeclaration) {
            const auto& function =
                static_cast<const FunctionDeclaration&>(declaration);
            if (!function.Owner().empty() &&
                !function.HasModifier(FunctionModifier::Private)) {
                const Declaration* owner = Lookup(
                    function.Owner(), function.moduleId, function.Range());
                if (owner) constraintApi = owner;
            }
        }
        for (const auto& [parameter, constraint] :
             declaration.genericConstraints) {
            (void)parameter;
            if (BuiltinTypeSet(constraint)) continue;
            const Declaration* set =
                LookupFrom(constraint, declaration, declaration.Range(), true);
            if (!set)
                Error(declaration.Range(),
                      "unknown type set '" + constraint + "'");
            else if (set->Kind() != AstNodeKind::TypeSetDeclaration)
                Error(declaration.Range(),
                      "'" + constraint + "' is not a type set");
            else if (constraintApi->exported && !set->exported)
                Error(declaration.Range(),
                      "exported generics cannot use non-exported type sets");
            else if (constraintApi->quiverVisible && !set->exported &&
                     !(set->quiverVisible &&
                       set->quiverId == constraintApi->quiverId))
                Error(declaration.Range(),
                      "quiver generics cannot use module-private type sets");
        }
        std::set<std::string> parameters(declaration.genericParameters.begin(),
                                         declaration.genericParameters.end());
        parameters.insert(declaration.ownerParameters.begin(),
                          declaration.ownerParameters.end());
        if (declaration.Kind() == AstNodeKind::StructureDeclaration &&
            (declaration.exported || declaration.quiverVisible))
            for (const auto& field :
                 static_cast<const StructureDeclaration&>(declaration).Fields())
                if (!field->IsPrivate())
                    ValidateApiType(field->Type(), declaration, parameters);
        if (declaration.Kind() == AstNodeKind::FunctionDeclaration) {
            const auto& function =
                static_cast<const FunctionDeclaration&>(declaration);
            const Declaration* api = function.exported || function.quiverVisible
                                         ? &function
                                         : nullptr;
            if (!function.Owner().empty()) {
                const auto* owner = Lookup(function.Owner(), function.moduleId,
                                           function.Range());
                if (function.exported || function.quiverVisible)
                    Error(function.Range(),
                          "member functions cannot have declaration "
                          "visibility; visibility comes from the owning type");
                api =
                    owner && (owner->exported || owner->quiverVisible) &&
                            !function.HasModifier(FunctionModifier::Private) &&
                            function.Name() != "tear"
                        ? owner
                        : nullptr;
            }
            if (api) {
                ValidateApiType(function.ReturnType(), *api, parameters);
                for (const auto& parameter : function.Parameters())
                    ValidateApiType(parameter->Type(), *api, parameters);
            }
        }
        if (declaration.Kind() == AstNodeKind::CallableTypeDeclaration &&
            (declaration.exported || declaration.quiverVisible)) {
            const auto& callable =
                static_cast<const CallableTypeDeclaration&>(declaration);
            for (const auto& parameter : callable.Parameters())
                ValidateApiType(parameter->Type(), declaration, parameters);
            for (const auto& result : callable.Results())
                ValidateApiType(*result, declaration, parameters);
        }
        Walk(declaration, [&](AstNode& node, bool enter) {
            if (!enter) return;
            std::string name;
            if (node.Kind() == AstNodeKind::Parameter)
                name = static_cast<Parameter&>(node).Name();
            if (node.Kind() == AstNodeKind::VariableDeclarationStatement)
                name = static_cast<VariableDeclarationStatement&>(node).Name();
            if (node.Kind() == AstNodeKind::DestructureBinding)
                name = static_cast<DestructureBinding&>(node).Name();
            if (!name.empty() && parameters.contains(name))
                Error(
                    node.Range(),
                    "value declaration shadows type parameter '" + name + "'");
        });
    }
    Declaration* Clone(const Declaration& origin, const Arguments& arguments,
                       const std::string& name, source::SourceRange at,
                       const std::string& owner = {}) {
        if (!ValidateConstraints(origin, arguments, at)) return nullptr;
        if (generatedDeclarations >= 2048 ||
            (current && current->instantiationOrigins.size() >= 64) ||
            generatedTokens + origin.templateTokens.size() > 1000000) {
            if (!failedBudget)
                Error(at,
                      "generic specialization budget exceeded (64 levels, 2048 "
                      "declarations, or 1000000 tokens)");
            failedBudget = true;
            return nullptr;
        }
        std::vector<std::string> parameters = origin.ownerParameters;
        parameters.insert(parameters.end(), origin.genericParameters.begin(),
                          origin.genericParameters.end());
        if (parameters.size() != arguments.size()) {
            Error(at,
                  "wrong number of type arguments for '" + Name(origin) + "'");
            return nullptr;
        }
        std::map<std::string, Tokens> substitutions;
        for (std::size_t i = 0; i < parameters.size(); ++i)
            substitutions[parameters[i]] = arguments[i];
        std::set<std::size_t> sites;
        std::map<std::size_t, const IsOfExpression*> isOfSites;
        Walk(origin, [&](AstNode& node, bool enter) {
            if (!enter) return;
            if (node.Kind() == AstNodeKind::IsOfExpression) {
                const auto& expression =
                    static_cast<const IsOfExpression&>(node);
                if (expression.Value().Kind() == AstNodeKind::NameExpression) {
                    const auto& receiver =
                        static_cast<const NameExpression&>(expression.Value());
                    if (substitutions.contains(receiver.Name()))
                        isOfSites[receiver.Range().start.offset] = &expression;
                }
            }
            std::string parameter;
            if (node.Kind() == AstNodeKind::Type)
                parameter = static_cast<TypeNode&>(node).Name();
            if (node.Kind() == AstNodeKind::NameExpression)
                parameter = static_cast<NameExpression&>(node).Name();
            if (!substitutions.contains(parameter)) return;
            for (const auto& token : origin.templateTokens)
                if (token.value == parameter &&
                    token.range.start.offset >= node.Range().start.offset &&
                    token.range.end.offset <= node.Range().end.offset)
                    sites.insert(token.range.start.offset);
        });
        Tokens tokens;
        for (std::size_t i = 0; i < origin.templateTokens.size(); ++i) {
            bool erase = false;
            for (const auto& [begin, end] : origin.parameterTokenRanges)
                if (i >= begin && i < end) erase = true;
            if (erase) continue;
            Token token = origin.templateTokens[i];
            // A type parameter is not a value expression after substitution
            // (`i32.isof<...>` cannot be parsed), so fold these queries while
            // cloning the specialization.
            if (const auto found = isOfSites.find(token.range.start.offset);
                found != isOfSites.end()) {
                const auto& query = *found->second;
                const auto& receiver =
                    static_cast<const NameExpression&>(query.Value());
                const bool member =
                    MatchesIsOfTarget(substitutions[receiver.Name()], query,
                                      origin, substitutions);
                token.type = member ? TokenType::True : TokenType::False;
                token.value = member ? "true" : "false";
                tokens.push_back(std::move(token));
                while (i + 1 < origin.templateTokens.size() &&
                       origin.templateTokens[i + 1].range.start.file ==
                           query.Range().start.file &&
                       origin.templateTokens[i + 1].range.end.offset <=
                           query.Range().end.offset)
                    ++i;
                continue;
            }
            if (i == origin.nameTokenIndex) {
                token.value = name;
                tokens.push_back(token);
            } else if (!owner.empty() && i == origin.ownerTokenIndex) {
                token.value = owner;
                tokens.push_back(token);
            } else if (sites.contains(token.range.start.offset) &&
                       substitutions.contains(token.value)) {
                for (auto argument : substitutions[token.value]) {
                    argument.range = token.range;
                    if (argument.type == TokenType::Const && !tokens.empty() &&
                        tokens.back().type == TokenType::Const)
                        continue;
                    tokens.push_back(std::move(argument));
                }
            } else
                tokens.push_back(token);
        }
        generatedTokens += tokens.size();
        ++generatedDeclarations;
        if (generatedTokens > 1000000) {
            Error(at, "generic specialization token budget exceeded");
            failedBudget = true;
            return nullptr;
        }
        tokens.emplace_back(TokenType::EOF_TOKEN, "", origin.Range());
        Parser parser(std::move(tokens), diagnostics,
                      "$generic" + std::to_string(generatedDeclarations) + "$");
        if (modules.contains(origin.sourceId)) {
            parser.SetImports(modules[origin.sourceId]->imports);
            parser.SetPrivateModule(modules[origin.sourceId]->privateModule);
        }
        auto parsed = parser.Parse()->TakeDeclarations();
        if (parsed.size() != 1) {
            Error(at, "could not specialize '" + Name(origin) + "'");
            return nullptr;
        }
        auto concrete = std::move(parsed.front());
        concrete->moduleId = origin.moduleId;
        concrete->sourceId = origin.sourceId;
        concrete->quiverId = origin.quiverId;
        // A concrete nominal type can appear in an exported ordinary signature
        // only when both its template and all nominal arguments are exported.
        // A quiver template, or an exported template applied to a quiver type,
        // keeps the matching quiver boundary. Module-private arguments keep the
        // specialization module-private.
        if (concrete->Kind() == AstNodeKind::StructureDeclaration ||
            concrete->Kind() == AstNodeKind::CallableTypeDeclaration) {
            bool exported = origin.exported;
            bool modulePrivate = !origin.exported && !origin.quiverVisible;
            std::string boundary = origin.quiverVisible ? origin.quiverId : "";
            for (const auto& argument : arguments)
                for (const auto& token : argument)
                    if (const auto alias =
                            unit.genericAliases.find(token.value);
                        alias != unit.genericAliases.end()) {
                        const auto& type = *alias->second;
                        if (!type.exported) exported = false;
                        if (!type.exported && !type.quiverVisible) {
                            modulePrivate = true;
                        } else if (type.quiverVisible) {
                            if (!boundary.empty() && boundary != type.quiverId)
                                modulePrivate = true;
                            else
                                boundary = type.quiverId;
                        }
                    }
            concrete->exported = exported;
            concrete->quiverVisible =
                !exported && !modulePrivate && !boundary.empty();
            if (concrete->quiverVisible) concrete->quiverId = boundary;
        }
        if (current)
            concrete->instantiationOrigins = current->instantiationOrigins;
        concrete->instantiationOrigins.push_back(at);
        concrete->specializationName = Name(origin) + "<";
        for (std::size_t i = 0; i < arguments.size(); ++i) {
            if (i) concrete->specializationName += ", ";
            for (const auto& token : arguments[i]) {
                const auto alias = unit.genericAliases.find(token.value);
                if (alias == unit.genericAliases.end())
                    concrete->specializationName += token.value;
                else
                    concrete->specializationName +=
                        alias->second->specializationName.empty()
                            ? Name(*alias->second)
                            : alias->second->specializationName;
            }
        }
        concrete->specializationName += ">";
        auto* result = concrete.get();
        if (result->Kind() == AstNodeKind::StructureDeclaration ||
            (result->Kind() == AstNodeKind::FunctionDeclaration &&
             static_cast<const FunctionDeclaration&>(*result).Owner().empty()))
            declarations[{result->moduleId, Name(*result)}] = result;
        unit.AddDeclaration(std::move(concrete));
        return result;
    }
    Declaration* Instantiate(const Declaration& origin,
                             const Arguments& arguments,
                             source::SourceRange at) {
        if (origin.genericParameters.empty() ||
            arguments.size() != origin.genericParameters.size()) {
            Error(at, "'" + Name(origin) + "' expects " +
                          std::to_string(origin.genericParameters.size()) +
                          " explicit type arguments");
            return nullptr;
        }
        const auto key = std::make_pair(&origin, Key(arguments));
        if (cache.contains(key)) return cache[key];
        auto* concrete =
            Clone(origin, arguments,
                  "$specialization" + std::to_string(cache.size()), at);
        if (!concrete) return nullptr;
        cache[key] = concrete;
        if (origin.Kind() == AstNodeKind::StructureDeclaration) {
            Instance instance{&origin, concrete, arguments};
            instances.push_back(instance);
            for (const auto* method : genericMethods[instance.origin])
                if (method->genericParameters.empty())
                    if (auto* clone = Clone(*method, arguments, method->Name(),
                                            at, Name(*concrete)))
                        concreteMethods[{concrete, method->Name()}] =
                            static_cast<FunctionDeclaration*>(clone);
        }
        return concrete;
    }
    void MaterializeMethod(const Instance& instance,
                           const MethodRequest& request) {
        for (const auto* method : genericMethods[instance.origin]) {
            if (method->Name() != request.name ||
                method->genericParameters.size() != request.arguments.size())
                continue;
            const auto key =
                std::make_pair(instance.concrete, request.concreteName);
            if (!methodCache.insert(key).second) continue;
            Arguments arguments = instance.arguments;
            arguments.insert(arguments.end(), request.arguments.begin(),
                             request.arguments.end());
            if (auto* clone = Clone(*method, arguments, request.concreteName,
                                    request.range, Name(*instance.concrete)))
                concreteMethods[{instance.concrete, request.concreteName}] =
                    static_cast<FunctionDeclaration*>(clone);
        }
    }
    struct TypeView {
        const TypeNode* syntax = nullptr;
        std::string module;
        int extraPointers = 0;
    };
    TypeView Element(TypeView value) {
        if (value.extraPointers > 0) {
            --value.extraPointers;
            return value;
        }
        if (!value.syntax || !value.syntax->Target()) return {};
        value.syntax = value.syntax->Target();
        return value;
    }
    const Declaration* ResolveName(NameExpression& name) {
        if (Local(name.Name())) return nullptr;
        const auto* origin = Lookup(name.Name(), current->moduleId,
                                    name.Range(), !name.typeArguments.empty());
        if (!name.typeArguments.empty()) {
            const auto arguments =
                ResolveArguments(name.typeArguments, current->moduleId);
            if (!origin)
                Error(name.Range(),
                      "unknown generic declaration '" + name.Name() + "'");
            else if (auto* concrete =
                         Instantiate(*origin, arguments, name.Range())) {
                name.Rename(Alias(*concrete));
                origin = concrete;
            }
            name.typeArguments.clear();
        }
        return origin;
    }
    const Declaration* Nominal(TypeView view) {
        if (!view.syntax) return nullptr;
        if (view.extraPointers ||
            view.syntax->TypeForm() == TypeKind::Reference ||
            view.syntax->TypeForm() == TypeKind::Pointer)
            view = Element(view);
        if (!view.syntax || view.extraPointers ||
            view.syntax->TypeForm() != TypeKind::Named)
            return nullptr;
        auto& type = const_cast<TypeNode&>(*view.syntax);
        if (!type.typeArguments.empty()) TypeTokens(type, view.module);
        const auto* declaration =
            Lookup(type.Name(), view.module, type.Range());
        return declaration &&
                       (declaration->Kind() ==
                            AstNodeKind::StructureDeclaration ||
                        declaration->Kind() == AstNodeKind::EnumDeclaration)
                   ? declaration
                   : nullptr;
    }
    const Declaration* Receiver(const MemberCallExpression& call) {
        if (call.Access() == TokenType::Scope &&
            call.Receiver().Kind() == AstNodeKind::NameExpression) {
            const auto* declaration = ResolveName(const_cast<NameExpression&>(
                static_cast<const NameExpression&>(call.Receiver())));
            return declaration &&
                           (declaration->Kind() ==
                                AstNodeKind::StructureDeclaration ||
                            declaration->Kind() == AstNodeKind::EnumDeclaration)
                       ? declaration
                       : nullptr;
        }
        return Nominal(ExpressionType(call.Receiver()));
    }
    std::unique_ptr<TypeNode> CopyType(const TypeNode& source) {
        std::unique_ptr<TypeNode> result;
        if (source.TypeForm() == TypeKind::Named)
            result = std::make_unique<TypeNode>(source.Range(), source.Name(),
                                                source.IsConst());
        else if (source.TypeForm() == TypeKind::Primitive)
            result = std::make_unique<TypeNode>(source.Range(), source.Type(),
                                                source.IsConst());
        else if (source.TypeForm() == TypeKind::Tuple) {
            std::vector<std::unique_ptr<TypeNode>> elements;
            for (const auto& type : source.Elements())
                elements.push_back(CopyType(*type));
            result = std::make_unique<TypeNode>(
                source.Range(), std::move(elements), source.IsConst());
        } else
            result = std::make_unique<TypeNode>(
                source.Range(), source.TypeForm(), CopyType(*source.Target()),
                source.IsConst(), source.QualifierRange(), source.Length());
        for (const auto& type : source.typeArguments)
            result->typeArguments.push_back(CopyType(*type));
        return result;
    }
    void AddBuiltinCandidates(const std::string& set, source::SourceRange range,
                              std::vector<std::unique_ptr<TypeNode>>& result) {
        const auto add = [&](PrimitiveType type) {
            result.push_back(std::make_unique<TypeNode>(range, type));
        };
        if (set == "signed_integer" || set == "integer" || set == "numeric") {
            add(PrimitiveType::I8);
            add(PrimitiveType::I16);
            add(PrimitiveType::I32);
            add(PrimitiveType::I64);
            add(PrimitiveType::ISize);
        }
        if (set == "unsigned_integer" || set == "integer" || set == "numeric") {
            add(PrimitiveType::U8);
            add(PrimitiveType::U16);
            add(PrimitiveType::U32);
            add(PrimitiveType::U64);
            add(PrimitiveType::USize);
        }
        if (set == "floating_point" || set == "numeric") {
            add(PrimitiveType::F32);
            add(PrimitiveType::F64);
        }
    }
    void CollectTypeSetCandidates(
        const TypeSetDeclaration& set,
        std::vector<std::unique_ptr<TypeNode>>& result,
        std::set<const Declaration*>& visiting) {
        if (!visiting.insert(&set).second) return;
        for (const auto& member : set.Members()) {
            if (member->TypeForm() == TypeKind::Named &&
                member->typeArguments.empty()) {
                if (BuiltinTypeSet(member->Name())) {
                    AddBuiltinCandidates(member->Name(), member->Range(),
                                         result);
                    continue;
                }
                const Declaration* nested =
                    LookupFrom(member->Name(), set, member->Range(), false);
                if (nested &&
                    nested->Kind() == AstNodeKind::TypeSetDeclaration) {
                    CollectTypeSetCandidates(
                        static_cast<const TypeSetDeclaration&>(*nested), result,
                        visiting);
                    continue;
                }
            }
            auto candidate = CopyType(*member);
            Declaration* saved = current;
            current = const_cast<TypeSetDeclaration*>(&set);
            TypeTokens(*candidate, set.moduleId);
            current = saved;
            result.push_back(std::move(candidate));
        }
        visiting.erase(&set);
    }
    void PrepareIsOf(IsOfExpression& expression) {
        std::vector<std::unique_ptr<TypeNode>> candidates;
        auto& target = expression.Target();
        if (target.TypeForm() == TypeKind::Named &&
            target.typeArguments.empty()) {
            if (BuiltinTypeSet(target.Name())) {
                AddBuiltinCandidates(target.Name(), target.Range(), candidates);
                expression.SetCandidates(std::move(candidates), true);
                return;
            }
            const Declaration* set =
                Lookup(target.Name(), current->moduleId, target.Range(), false);
            if (set && set->Kind() == AstNodeKind::TypeSetDeclaration) {
                std::set<const Declaration*> visiting;
                CollectTypeSetCandidates(
                    static_cast<const TypeSetDeclaration&>(*set), candidates,
                    visiting);
                expression.SetCandidates(std::move(candidates), true);
                return;
            }
        }
        auto candidate = CopyType(target);
        TypeTokens(*candidate, current->moduleId);
        candidates.push_back(std::move(candidate));
        expression.SetCandidates(std::move(candidates), false);
    }
    bool MatchesIsOfTarget(const Tokens& argument,
                           const IsOfExpression& expression,
                           const Declaration& origin,
                           const std::map<std::string, Tokens>& substitutions) {
        const TypeNode& target = expression.Target();
        if (target.TypeForm() == TypeKind::Named &&
            target.typeArguments.empty()) {
            if (const auto substituted = substitutions.find(target.Name());
                substituted != substitutions.end())
                return Key({argument}) == Key({substituted->second});
            if (BuiltinTypeSet(target.Name()))
                return Satisfies(argument, target.Name(), origin,
                                 target.Range());
            const Declaration* set =
                LookupFrom(target.Name(), origin, target.Range(), false);
            if (set && set->Kind() == AstNodeKind::TypeSetDeclaration)
                return Satisfies(argument, target.Name(), origin,
                                 target.Range());
        }
        auto concrete = CopyType(target);
        Declaration* saved = current;
        current = const_cast<Declaration*>(&origin);
        Tokens targetTokens = TypeTokens(*concrete, origin.moduleId);
        current = saved;
        return Key({argument}) == Key({targetTokens});
    }
    std::unique_ptr<TypeNode> CopyView(TypeView view,
                                       source::SourceRange range) {
        if (!view.syntax) return nullptr;
        auto result = CopyType(*view.syntax);
        // Canonicalize in the declaration's module before using the type in a
        // caller expression (which may have different import aliases).
        TypeTokens(*result, view.module);
        for (int i = 0; i < view.extraPointers; ++i)
            result = std::make_unique<TypeNode>(range, TypeKind::Pointer,
                                                std::move(result));
        return result;
    }
    TypeView ExpressionType(const Expression& expression) {
        switch (expression.Kind()) {
            case AstNodeKind::NameExpression: {
                const auto& name =
                    static_cast<const NameExpression&>(expression).Name();
                for (auto scope = localTypes.rbegin();
                     scope != localTypes.rend(); ++scope)
                    if (const auto found = scope->find(name);
                        found != scope->end())
                        return found->second;
                return {};
            }
            case AstNodeKind::ArrayExpression:
            case AstNodeKind::TupleExpression: {
                if (!expressionTypes.contains(&expression)) {
                    const auto& elements =
                        expression.Kind() == AstNodeKind::ArrayExpression
                            ? static_cast<const ArrayExpression&>(expression)
                                  .Elements()
                            : static_cast<const TupleExpression&>(expression)
                                  .Elements();
                    if (elements.empty()) return {};
                    std::vector<std::unique_ptr<TypeNode>> types;
                    for (const auto& element : elements) {
                        auto type = CopyView(ExpressionType(*element),
                                             element->Range());
                        // This view only tracks nominal receiver identities;
                        // scalar tuple elements need no inferred semantic type.
                        if (!type)
                            type = std::make_unique<TypeNode>(
                                element->Range(), PrimitiveType::Void);
                        types.push_back(std::move(type));
                    }
                    if (expression.Kind() == AstNodeKind::TupleExpression)
                        expressionTypes[&expression] =
                            std::make_unique<TypeNode>(expression.Range(),
                                                       std::move(types));
                    else
                        expressionTypes[&expression] =
                            std::make_unique<TypeNode>(
                                expression.Range(), TypeKind::Array,
                                std::move(types.front()), false,
                                source::SourceRange{}, elements.size());
                }
                return {expressionTypes[&expression].get(), current->moduleId};
            }
            case AstNodeKind::SliceExpression: {
                const auto view = Element(ExpressionType(
                    static_cast<const SliceExpression&>(expression)
                        .Receiver()));
                auto element = CopyView(view, expression.Range());
                if (!element) return {};
                if (!expressionTypes.contains(&expression))
                    expressionTypes[&expression] = std::make_unique<TypeNode>(
                        expression.Range(), TypeKind::Slice,
                        std::move(element));
                return {expressionTypes[&expression].get(), current->moduleId};
            }
            case AstNodeKind::ParenthesizedExpression:
                return ExpressionType(
                    static_cast<const ParenthesizedExpression&>(expression)
                        .Inner());
            case AstNodeKind::MatchExpression: {
                const auto& match =
                    static_cast<const MatchExpression&>(expression);
                return match.Arms().empty()
                           ? TypeView{}
                           : ExpressionType(match.Arms().front()->Value());
            }
            case AstNodeKind::BuildExpression:
                return {&static_cast<const BuildExpression&>(expression).Type(),
                        current->moduleId};
            case AstNodeKind::CastExpression:
                return {&static_cast<const CastExpression&>(expression)
                             .TargetType(),
                        current->moduleId};
            case AstNodeKind::AssignmentExpression:
                return ExpressionType(
                    static_cast<const AssignmentExpression&>(expression)
                        .Target());
            case AstNodeKind::UnaryExpression: {
                const auto& unary =
                    static_cast<const UnaryExpression&>(expression);
                auto value = ExpressionType(unary.Operand());
                if (unary.OperatorType() == TokenType::Mul)
                    return Element(value);
                if (unary.OperatorType() == TokenType::Ampersand ||
                    unary.OperatorType() == TokenType::RawAddress)
                    ++value.extraPointers;
                return value;
            }
            case AstNodeKind::IndexExpression:
                return Element(ExpressionType(
                    static_cast<const IndexExpression&>(expression)
                        .Receiver()));
            case AstNodeKind::CallExpression: {
                const auto& call =
                    static_cast<const CallExpression&>(expression);
                if (call.Callee().Kind() != AstNodeKind::NameExpression)
                    return {};
                if (call.TypeArgument()) {
                    if (!expressionTypes.contains(&expression)) {
                        const auto& element =
                            call.TypeArgument()->TypeForm() == TypeKind::Slice
                                ? *call.TypeArgument()->Target()
                                : *call.TypeArgument();
                        std::vector<std::unique_ptr<TypeNode>> values;
                        values.push_back(std::make_unique<TypeNode>(
                            expression.Range(), TypeKind::Pointer,
                            CopyType(element)));
                        values.push_back(std::make_unique<TypeNode>(
                            expression.Range(), PrimitiveType::ErrorValue));
                        expressionTypes[&expression] =
                            std::make_unique<TypeNode>(expression.Range(),
                                                       std::move(values));
                    }
                    return {expressionTypes[&expression].get(),
                            current->moduleId};
                }
                const auto* declaration =
                    ResolveName(const_cast<NameExpression&>(
                        static_cast<const NameExpression&>(call.Callee())));
                if (!declaration &&
                    static_cast<const NameExpression&>(call.Callee()).Name() ==
                        "take" &&
                    call.Arguments().size() == 1)
                    return ExpressionType(*call.Arguments()[0]);
                if (declaration &&
                    declaration->Kind() == AstNodeKind::FunctionDeclaration)
                    return {
                        &static_cast<const FunctionDeclaration*>(declaration)
                             ->ReturnType(),
                        declaration->moduleId};
                return {};
            }
            case AstNodeKind::MemberCallExpression: {
                const auto& call =
                    static_cast<const MemberCallExpression&>(expression);
                if (call.Member() == "front" || call.Member() == "back") {
                    const auto type = ExpressionType(call.Receiver());
                    if (type.syntax &&
                        (type.syntax->TypeForm() == TypeKind::Array ||
                         type.syntax->TypeForm() == TypeKind::Slice))
                        return Element(type);
                }
                if (call.Member() == "data") {
                    auto type = ExpressionType(call.Receiver());
                    if (type.syntax &&
                        type.syntax->TypeForm() == TypeKind::Slice) {
                        type = Element(type);
                        ++type.extraPointers;
                        return type;
                    }
                }
                const auto* owner = Receiver(call);
                if (!owner) return {};
                const auto found = concreteMethods.find({owner, call.Member()});
                if (found != concreteMethods.end())
                    return {&found->second->ReturnType(),
                            found->second->moduleId};
                if (call.Member() == "init" &&
                    owner->Kind() == AstNodeKind::StructureDeclaration) {
                    if (!ownerTypes.contains(owner))
                        ownerTypes[owner] = std::make_unique<TypeNode>(
                            owner->Range(), Alias(*owner));
                    return {ownerTypes[owner].get(), owner->moduleId};
                }
                return {};
            }
            case AstNodeKind::MemberAccessExpression: {
                const auto& member =
                    static_cast<const MemberAccessExpression&>(expression);
                const auto* owner = Nominal(ExpressionType(member.Receiver()));
                if (!owner ||
                    owner->Kind() != AstNodeKind::StructureDeclaration)
                    return {};
                const auto* structure =
                    static_cast<const StructureDeclaration*>(owner);
                for (const auto& field : structure->Fields())
                    if (field->Name() == member.Member())
                        return {&field->Type(), structure->moduleId};
                return {};
            }
            default:
                return {};
        }
    }
    void CheckTypeNames(const TypeNode& type) {
        // Function signatures resolve at module scope; body applications must
        // honor local shadowing just like ordinary type syntax.
        if (locals.size() <= 1) return;
        Walk(type, [&](AstNode& node, bool enter) {
            if (!enter || node.Kind() != AstNodeKind::Type) return;
            const auto& named = static_cast<const TypeNode&>(node);
            if (named.TypeForm() == TypeKind::Named && Local(named.Name()))
                Error(named.Range(),
                      "a local value cannot be a generic type argument");
        });
    }
    void Rewrite(AstNode& node, bool enter) {
        if (failedBudget) return;
        if (enter && node.Kind() == AstNodeKind::IsOfExpression)
            PrepareIsOf(static_cast<IsOfExpression&>(node));
        if (node.Kind() == AstNodeKind::BuildExpression) {
            if (enter) {
                locals.emplace_back();
                localTypes.emplace_back();
                locals.back().insert("self");
                localTypes.back()["self"] = {
                    &static_cast<BuildExpression&>(node).Type(),
                    current->moduleId};
            } else {
                locals.pop_back();
                localTypes.pop_back();
            }
        }
        if (node.Kind() == AstNodeKind::BlockStatement) {
            if (enter) {
                locals.emplace_back();
                localTypes.emplace_back();
            } else {
                locals.pop_back();
                localTypes.pop_back();
            }
        }
        if (!enter) {
            if (node.Kind() == AstNodeKind::MemberCallExpression)
                RewriteMethod(static_cast<MemberCallExpression&>(node));
            if (node.Kind() == AstNodeKind::VariableDeclarationStatement) {
                const auto& variable =
                    static_cast<VariableDeclarationStatement&>(node);
                TypeView type =
                    variable.ExplicitType()
                        ? TypeView{variable.ExplicitType(), current->moduleId}
                    : variable.Initializer()
                        ? ExpressionType(*variable.Initializer())
                        : TypeView{};
                locals.back().insert(variable.Name());
                localTypes.back()[variable.Name()] = type;
            }
            if (node.Kind() == AstNodeKind::Parameter) {
                const auto& parameter = static_cast<Parameter&>(node);
                locals.back().insert(parameter.Name());
                localTypes.back()[parameter.Name()] = {&parameter.Type(),
                                                       current->moduleId};
            }
            if (node.Kind() == AstNodeKind::DestructureStatement) {
                const auto& destructure =
                    static_cast<DestructureStatement&>(node);
                const auto type = ExpressionType(destructure.Value());
                for (std::size_t i = 0; i < destructure.Bindings().size();
                     ++i) {
                    const auto& binding = *destructure.Bindings()[i];
                    if (binding.Name() == "_") continue;
                    bool local = locals.back().contains(binding.Name());
                    // Parameters and the outermost function body share one
                    // semantic scope even though the syntax walker keeps a
                    // frame for each.
                    if (!local && locals.size() == 2)
                        local = locals.front().contains(binding.Name());
                    const bool declaration =
                        binding.ExplicitType() != nullptr ||
                        (destructure.IsInferred() && !local);
                    if (!declaration) continue;
                    locals.back().insert(binding.Name());
                    if (binding.ExplicitType())
                        localTypes.back()[binding.Name()] = {
                            binding.ExplicitType(), current->moduleId};
                    else if (type.syntax &&
                             type.syntax->TypeForm() == TypeKind::Tuple &&
                             i < type.syntax->Elements().size())
                        localTypes.back()[binding.Name()] = {
                            type.syntax->Elements()[i].get(), type.module};
                }
            }
            return;
        }
        if (node.Kind() == AstNodeKind::Type) {
            auto& type = static_cast<TypeNode&>(node);
            if (!type.typeArguments.empty()) {
                CheckTypeNames(type);
                TypeTokens(type, current->moduleId);
            } else if (type.TypeForm() == TypeKind::Named) {
                const auto* found =
                    Lookup(type.Name(), current->moduleId, type.Range(), false);
                if (found && !found->genericParameters.empty())
                    Error(type.Range(),
                          "generic type requires explicit type arguments");
            }
        }
        if (node.Kind() == AstNodeKind::NameExpression) {
            auto& name = static_cast<NameExpression&>(node);
            if (IsPrimitiveNumericTypeName(name.Name())) return;
            if (Local(name.Name())) {
                if (!name.typeArguments.empty())
                    Error(name.Range(),
                          "a local value cannot have type arguments");
                return;
            }
            const auto* origin =
                Lookup(name.Name(), current->moduleId, name.Range(),
                       !name.typeArguments.empty());
            if (!name.typeArguments.empty()) {
                for (const auto& argument : name.typeArguments)
                    CheckTypeNames(*argument);
                auto arguments =
                    ResolveArguments(name.typeArguments, current->moduleId);
                if (!origin)
                    Error(name.Range(),
                          "unknown generic declaration '" + name.Name() + "'");
                else if (auto* concrete =
                             Instantiate(*origin, arguments, name.Range()))
                    name.Rename(Alias(*concrete));
                name.typeArguments.clear();
            } else if (origin && !origin->genericParameters.empty())
                Error(name.Range(), "generic declaration '" + name.Name() +
                                        "' requires explicit type arguments");
        }
    }
    void RewriteMethod(MemberCallExpression& call) {
        if (call.typeArguments.empty()) return;
        for (const auto& argument : call.typeArguments)
            CheckTypeNames(*argument);
        const auto arguments =
            ResolveArguments(call.typeArguments, current->moduleId);
        const auto key = call.Member() + Key(arguments);
        const auto* owner = Receiver(call);
        if (!owner) {
            Error(call.Range(),
                  "cannot resolve receiver type for generic method '" +
                      call.Member() + "'");
            return;
        }
        if (!requestNames.contains(key))
            requestNames[key] =
                "$generic_method" + std::to_string(requestNames.size());
        const MethodRequest request{call.Member(), requestNames[key], arguments,
                                    call.Range()};
        bool found = false;
        for (const auto& instance : instances)
            if (instance.concrete == owner) {
                MaterializeMethod(instance, request);
                found = concreteMethods.contains({owner, request.concreteName});
                break;
            }
        if (!found)
            Error(call.Range(),
                  "unknown generic method or wrong type argument count for '" +
                      call.Member() + "'");
        call.Rename(requestNames[key]);
        call.typeArguments.clear();
    }
    CompilationUnit& unit;
    diagnostics::DiagnosticBag& diagnostics;
    Declaration* current = nullptr;
    bool failedBudget = false;
    std::size_t generatedTokens = 0, generatedDeclarations = 0;
    std::map<std::string, const Module*> modules;
    std::map<std::pair<std::string, std::string>, const Declaration*>
        declarations;
    std::vector<const FunctionDeclaration*> methods;
    std::map<const Declaration*, std::vector<const FunctionDeclaration*>>
        genericMethods;
    std::map<const Declaration*, std::string> aliases;
    std::map<std::pair<const Declaration*, std::string>, Declaration*> cache;
    std::vector<Instance> instances;
    std::map<std::string, std::string> requestNames;
    std::set<std::pair<const Declaration*, std::string>> methodCache;
    std::vector<std::set<std::string>> locals;
    std::vector<std::map<std::string, TypeView>> localTypes;
    std::map<std::pair<const Declaration*, std::string>,
             const FunctionDeclaration*>
        concreteMethods;
    std::map<const Declaration*, std::unique_ptr<TypeNode>> ownerTypes;
    std::map<const Expression*, std::unique_ptr<TypeNode>> expressionTypes;
};
}  // namespace
void ExpandGenerics(core::CompilationUnit& unit,
                    diagnostics::DiagnosticBag& diagnostics) {
    Expander(unit, diagnostics).Run();
}
void ReportGenericOrigins(const core::CompilationUnit& unit,
                          diagnostics::DiagnosticBag& diagnostics) {
    const auto errors = diagnostics.Diagnostics();
    for (const auto& declaration : unit.Declarations()) {
        if (declaration->instantiationOrigins.empty()) continue;
        const bool affected =
            std::any_of(errors.begin(), errors.end(), [&](const auto& error) {
                return error.severity ==
                           diagnostics::DiagnosticSeverity::Error &&
                       error.range.start.file ==
                           declaration->Range().start.file &&
                       error.range.start.offset >=
                           declaration->Range().start.offset &&
                       error.range.start.offset <=
                           declaration->Range().end.offset;
            });
        if (affected)
            for (const auto& origin : declaration->instantiationOrigins)
                diagnostics.Report(DiagnosticCode::InvalidGeneric, origin,
                                   "while instantiating '" +
                                       declaration->specializationName + "'",
                                   diagnostics::DiagnosticSeverity::Note);
    }
}
}  // namespace flecha::semantic
