#include "core/parser.hpp"

#include <algorithm>
#include <charconv>
#include <string>
#include <utility>

namespace flecha::core {
namespace {

std::unique_ptr<TypeNode> CloneType(const TypeNode& type) {
    if (type.TypeForm() == TypeKind::Named)
        return std::make_unique<TypeNode>(
            type.Range(), type.Name(), type.IsConst(), type.QualifierRange());
    if (type.TypeForm() == TypeKind::Tuple) {
        std::vector<std::unique_ptr<TypeNode>> elements;
        for (const auto& element : type.Elements())
            elements.push_back(CloneType(*element));
        return std::make_unique<TypeNode>(type.Range(), std::move(elements),
                                          type.IsConst(),
                                          type.QualifierRange());
    }
    if (type.TypeForm() == TypeKind::Primitive)
        return std::make_unique<TypeNode>(
            type.Range(), type.Type(), type.IsConst(), type.QualifierRange());
    return std::make_unique<TypeNode>(type.Range(), type.TypeForm(),
                                      CloneType(*type.Target()), type.IsConst(),
                                      type.QualifierRange(), type.Length());
}

source::SourceRange EndOfInputRange(const std::vector<Token>& tokens) {
    if (tokens.empty()) {
        const source::SourceLocation origin{};
        return {origin, origin};
    }

    const source::SourceLocation end = tokens.back().range.end;
    return {end, end};
}

}  // namespace

Parser::Parser(std::vector<Token> tokens,
               diagnostics::DiagnosticBag& diagnostics)
    : _tokens(std::move(tokens)), _diagnostics(diagnostics) {
    if (_tokens.empty() || _tokens.back().type != TokenType::EOF_TOKEN) {
        _tokens.emplace_back(TokenType::EOF_TOKEN, "",
                             EndOfInputRange(_tokens));
    }
}

const Token& Parser::Current() const { return Peek(0); }

const Token& Parser::Peek(std::size_t offset) const {
    const std::size_t index = std::min(_position + offset, _tokens.size() - 1);
    return _tokens[index];
}

bool Parser::IsAtEnd() const { return Current().type == TokenType::EOF_TOKEN; }

bool Parser::Check(TokenType type) const { return Current().type == type; }

bool Parser::IsInferredVariableDeclaration() const {
    return Check(TokenType::Identifier) &&
           Peek(1).type == TokenType::DeclareAssign;
}

const Token& Parser::Advance() {
    const Token& current = Current();
    if (!IsAtEnd()) {
        ++_position;
    }
    return current;
}

bool Parser::Match(TokenType type) {
    if (!Check(type)) {
        return false;
    }
    Advance();
    return true;
}

bool Parser::IsTypeToken(TokenType type) const {
    switch (type) {
        case TokenType::I8:
        case TokenType::I16:
        case TokenType::I32:
        case TokenType::I64:
        case TokenType::ISize:
        case TokenType::U8:
        case TokenType::U16:
        case TokenType::U32:
        case TokenType::U64:
        case TokenType::USize:
        case TokenType::F32:
        case TokenType::F64:
        case TokenType::Bool:
        case TokenType::Char:
        case TokenType::Void:
        case TokenType::String:
        case TokenType::StringView:
        case TokenType::Error:
            return true;
        default:
            return false;
    }
}

bool Parser::StartsType() const {
    // Preserve the contextual raw-address operator before named-type lookahead.
    if (Check(TokenType::Ampersand) && Peek(1).type == TokenType::Identifier &&
        Peek(1).value == "raw")
        return false;
    if (Check(TokenType::Own) || Check(TokenType::Lend) ||
        Check(TokenType::Const) || Check(TokenType::ConstPtr) ||
        Check(TokenType::ConstRef))
        return true;
    std::size_t offset = 0;
    for (;;) {
        const TokenType token = Peek(offset).type;
        if (token == TokenType::Mul || token == TokenType::Ampersand ||
            token == TokenType::Const || token == TokenType::ConstPtr ||
            token == TokenType::ConstRef || token == TokenType::LParen) {
            ++offset;
        } else if (token == TokenType::LBracket) {
            // Look through the bracketed count to distinguish [N]T from an
            // array literal expression, including malformed count syntax.
            ++offset;
            while (Peek(offset).type != TokenType::RBracket) {
                const TokenType next = Peek(offset).type;
                if (next == TokenType::EOF_TOKEN ||
                    next == TokenType::SemiColon || next == TokenType::LCurly ||
                    next == TokenType::RCurly)
                    return false;
                ++offset;
            }
            ++offset;
        } else
            break;
    }
    if (IsTypeToken(Peek(offset).type)) return true;
    offset = 0;
    return ScanType(offset) && Peek(offset).type == TokenType::Identifier;
}

bool Parser::ScanType(std::size_t& offset) const {
    if (Peek(offset).type == TokenType::Own ||
        Peek(offset).type == TokenType::Lend)
        ++offset;
    if (Peek(offset).type == TokenType::Const) ++offset;
    const TokenType token = Peek(offset).type;
    if (token == TokenType::Mul || token == TokenType::Ampersand ||
        token == TokenType::ConstPtr || token == TokenType::ConstRef) {
        ++offset;
        return ScanType(offset);
    }
    if (token == TokenType::LBracket) {
        ++offset;
        if (Peek(offset).type == TokenType::NumberLiteral) ++offset;
        if (Peek(offset).type != TokenType::RBracket) return false;
        ++offset;
        return ScanType(offset);
    }
    if (token == TokenType::LParen) {
        ++offset;
        if (!ScanType(offset)) return false;
        if (Peek(offset).type != TokenType::Comma) return false;
        do {
            ++offset;
            if (Peek(offset).type == TokenType::RParen) break;
            if (!ScanType(offset)) return false;
        } while (Peek(offset).type == TokenType::Comma);
        if (Peek(offset).type != TokenType::RParen) return false;
        ++offset;
        return true;
    }
    if (IsTypeToken(token) || token == TokenType::Identifier) {
        ++offset;
        if (token == TokenType::Identifier &&
            _importAliases.contains(Peek(offset - 1).value) &&
            Peek(offset).type == TokenType::Scope &&
            Peek(offset + 1).type == TokenType::Identifier)
            offset += 2;
        if (token == TokenType::Identifier &&
            Peek(offset).type == TokenType::Less)
            return ScanTypeArguments(offset);
        return true;
    }
    return false;
}

bool Parser::ScanTypeArguments(std::size_t& offset) const {
    if (Peek(offset).type != TokenType::Less) return false;
    ++offset;
    if (!ScanType(offset)) return false;
    while (Peek(offset).type == TokenType::Comma) {
        ++offset;
        if (!ScanType(offset)) return false;
    }
    if (Peek(offset).type != TokenType::Greater) return false;
    ++offset;
    return true;
}

std::vector<std::unique_ptr<TypeNode>> Parser::ParseTypeArguments() {
    std::vector<std::unique_ptr<TypeNode>> arguments;
    if (!Match(TokenType::Less)) return arguments;
    do {
        auto argument = ParseType();
        if (!argument) break;
        arguments.push_back(std::move(argument));
    } while (Match(TokenType::Comma));
    Consume(TokenType::Greater, "'>' after type arguments");
    return arguments;
}

std::vector<std::string> Parser::ParseGenericParameters() {
    std::vector<std::string> parameters;
    if (!Match(TokenType::Less)) return parameters;
    do {
        const auto* parameter =
            Consume(TokenType::Identifier, "type parameter name");
        if (!parameter) break;
        if (std::find(parameters.begin(), parameters.end(), parameter->value) !=
            parameters.end())
            _diagnostics.Report(
                diagnostics::DiagnosticCode::DuplicateDeclaration,
                parameter->range,
                "duplicate generic type parameter '" + parameter->value + "'");
        parameters.push_back(parameter->value);
    } while (Match(TokenType::Comma));
    Consume(TokenType::Greater, "'>' after type parameters");
    return parameters;
}

const Token* Parser::Consume(TokenType type, std::string_view expectation) {
    if (Check(type)) {
        return &Advance();
    }

    _diagnostics.Report(diagnostics::DiagnosticCode::ExpectedToken,
                        Current().range,
                        "expected " + std::string(expectation) + ", found '" +
                            Current().value + "'");
    return nullptr;
}

void Parser::ReportUnexpectedToken(std::string_view context) {
    const std::string tokenText =
        Current().value.empty() ? "end of file" : "'" + Current().value + "'";
    _diagnostics.Report(diagnostics::DiagnosticCode::UnexpectedToken,
                        Current().range,
                        "unexpected " + tokenText + " " + std::string(context));
}

std::unique_ptr<TypeNode> Parser::ParseType(bool allowOwn, bool allowLend,
                                            bool allowTupleOwners) {
    if (Check(TokenType::Own) || Check(TokenType::Lend)) {
        const Token contract = Advance();
        auto type = ParseType();
        if (!type) return nullptr;
        if (type->TypeForm() != TypeKind::Pointer ||
            (contract.type == TokenType::Own ? !allowOwn : !allowLend)) {
            _diagnostics.Report(
                diagnostics::DiagnosticCode::UnexpectedToken, contract.range,
                "ownership contracts require a raw pointer parameter, owning "
                "return, or owning field; references never own or lend");
            return nullptr;
        }
        type->SetContract(contract.type == TokenType::Own
                              ? OwnershipContract::Own
                              : OwnershipContract::Lend);
        return type;
    }
    const source::SourceLocation start = Current().range.start;
    source::SourceRange qualifierRange;
    const bool explicitConst = Check(TokenType::Const);
    if (explicitConst) qualifierRange = Advance().range;
    const Token& token = Current();
    const bool alias =
        token.type == TokenType::ConstPtr || token.type == TokenType::ConstRef;
    if (explicitConst && alias) {
        _diagnostics.Report(diagnostics::DiagnosticCode::UnexpectedToken,
                            token.range, "duplicate const qualifier");
        return nullptr;
    }
    if (token.type == TokenType::Mul || token.type == TokenType::Ampersand ||
        alias) {
        Advance();
        std::unique_ptr<TypeNode> target = ParseType();
        if (target == nullptr) return nullptr;
        const source::SourceRange range{start, target->Range().end};
        const TypeKind kind =
            token.type == TokenType::Mul || token.type == TokenType::ConstPtr
                ? TypeKind::Pointer
                : TypeKind::Reference;
        return std::make_unique<TypeNode>(range, kind, std::move(target),
                                          explicitConst || alias,
                                          alias ? token.range : qualifierRange);
    }
    if (Match(TokenType::LBracket)) {
        if (Match(TokenType::RBracket)) {
            auto target = ParseType();
            if (target == nullptr) return nullptr;
            const source::SourceRange range{start, target->Range().end};
            return std::make_unique<TypeNode>(range, TypeKind::Slice,
                                              std::move(target), explicitConst,
                                              qualifierRange);
        }
        const std::optional<std::uint64_t> length = ParseArrayLength();
        if (!length.has_value() ||
            Consume(TokenType::RBracket, "']' after array length") == nullptr)
            return nullptr;
        std::unique_ptr<TypeNode> target = ParseType();
        if (target == nullptr) return nullptr;
        const source::SourceRange range{start, target->Range().end};
        return std::make_unique<TypeNode>(range, TypeKind::Array,
                                          std::move(target), explicitConst,
                                          qualifierRange, *length);
    }
    if (Match(TokenType::LParen)) {
        std::vector<std::unique_ptr<TypeNode>> elements;
        do {
            auto element = ParseType(allowTupleOwners, false, allowTupleOwners);
            if (element == nullptr) return nullptr;
            elements.push_back(std::move(element));
        } while (Match(TokenType::Comma) && !Check(TokenType::RParen));
        const Token* close = Consume(TokenType::RParen, "')' after tuple type");
        if (close == nullptr) return nullptr;
        if (elements.size() < 2) {
            ReportUnexpectedToken(
                "after tuple type: at least two elements are required");
            return nullptr;
        }
        return std::make_unique<TypeNode>(
            source::SourceRange{start, close->range.end}, std::move(elements),
            explicitConst, qualifierRange);
    }
    if (Match(TokenType::Identifier)) {
        std::string name = token.value;
        auto end = token.range.end;
        if (_importAliases.contains(name) && Match(TokenType::Scope)) {
            const Token* member = Consume(TokenType::Identifier,
                                          "type name after module qualifier");
            if (!member) return nullptr;
            name += "::" + member->value;
            end = member->range.end;
        }
        auto type =
            std::make_unique<TypeNode>(source::SourceRange{start, end}, name,
                                       explicitConst, qualifierRange);
        if (Check(TokenType::Less)) type->typeArguments = ParseTypeArguments();
        return type;
    }
    PrimitiveType type;

    switch (token.type) {
        case TokenType::Error:
            type = PrimitiveType::ErrorValue;
            break;
        case TokenType::I8:
            type = PrimitiveType::I8;
            break;
        case TokenType::I16:
            type = PrimitiveType::I16;
            break;
        case TokenType::I32:
            type = PrimitiveType::I32;
            break;
        case TokenType::I64:
            type = PrimitiveType::I64;
            break;
        case TokenType::ISize:
            type = PrimitiveType::ISize;
            break;
        case TokenType::U8:
            type = PrimitiveType::U8;
            break;
        case TokenType::U16:
            type = PrimitiveType::U16;
            break;
        case TokenType::U32:
            type = PrimitiveType::U32;
            break;
        case TokenType::U64:
            type = PrimitiveType::U64;
            break;
        case TokenType::USize:
            type = PrimitiveType::USize;
            break;
        case TokenType::F32:
            type = PrimitiveType::F32;
            break;
        case TokenType::F64:
            type = PrimitiveType::F64;
            break;
        case TokenType::Bool:
            type = PrimitiveType::Bool;
            break;
        case TokenType::Char:
            type = PrimitiveType::Char;
            break;
        case TokenType::Void:
            type = PrimitiveType::Void;
            break;
        case TokenType::String:
            type = PrimitiveType::String;
            break;
        case TokenType::StringView:
            type = PrimitiveType::StringView;
            break;
        default:
            _diagnostics.Report(diagnostics::DiagnosticCode::ExpectedToken,
                                token.range, "expected a type");
            return nullptr;
    }

    Advance();
    return std::make_unique<TypeNode>(
        source::SourceRange{start, token.range.end}, type, explicitConst,
        qualifierRange);
}

std::unique_ptr<Parameter> Parser::ParseParameter() {
    const source::SourceLocation start = Current().range.start;
    std::unique_ptr<TypeNode> type = ParseType(true, true);
    if (type == nullptr) {
        return nullptr;
    }

    const Token* name = Consume(TokenType::Identifier, "parameter name");
    if (name == nullptr) {
        return nullptr;
    }

    return std::make_unique<Parameter>(
        source::SourceRange{start, name->range.end}, std::move(type),
        name->value, name->range);
}

std::unique_ptr<Parameter> Parser::ParseCallableParameter() {
    const source::SourceLocation start = Current().range.start;
    std::unique_ptr<TypeNode> type = ParseType(true, true, true);
    if (type == nullptr) return nullptr;

    std::string name;
    source::SourceRange nameRange = type->Range();
    if (Check(TokenType::Identifier)) {
        const Token& token = Advance();
        name = token.value;
        nameRange = token.range;
    }
    return std::make_unique<Parameter>(
        source::SourceRange{start, nameRange.end}, std::move(type),
        std::move(name), nameRange);
}

std::unique_ptr<CallableTypeDeclaration>
Parser::ParseCallableTypeDeclaration() {
    const source::SourceLocation start = Current().range.start;
    const bool isUnsafe = Match(TokenType::Unsafe);
    if (Consume(TokenType::Func, "'func'") == nullptr) return nullptr;
    const Token* name = Consume(TokenType::Identifier, "callable type name");
    if (name == nullptr ||
        Consume(TokenType::LParen, "'(' after callable type name") == nullptr)
        return nullptr;

    std::vector<std::unique_ptr<Parameter>> parameters;
    while (!Check(TokenType::RParen) && !IsAtEnd()) {
        auto parameter = ParseCallableParameter();
        if (parameter == nullptr) return nullptr;
        parameters.push_back(std::move(parameter));
        if (!Match(TokenType::Comma)) break;
    }
    if (Consume(TokenType::RParen, "')' after callable parameters") == nullptr)
        return nullptr;
    if (Consume(TokenType::FatArrow, "'=>' after callable parameters") ==
        nullptr)
        return nullptr;
    if (Consume(TokenType::LParen, "'(' before callable results") == nullptr)
        return nullptr;

    std::vector<std::unique_ptr<TypeNode>> results;
    while (!Check(TokenType::RParen) && !IsAtEnd()) {
        auto result = ParseType(true, false, true);
        if (result == nullptr) return nullptr;
        results.push_back(std::move(result));
        if (!Match(TokenType::Comma)) break;
    }
    if (Consume(TokenType::RParen, "')' after callable results") == nullptr)
        return nullptr;

    std::string borrowSource;
    source::SourceRange borrowSourceRange;
    if (Match(TokenType::LeftArrow)) {
        const Token* source =
            Consume(TokenType::Identifier, "borrow source after '<-'");
        if (source == nullptr) return nullptr;
        borrowSource = source->value;
        borrowSourceRange = source->range;
    }
    const Token* semicolon =
        Consume(TokenType::SemiColon, "';' after callable type declaration");
    if (semicolon == nullptr) return nullptr;
    return std::make_unique<CallableTypeDeclaration>(
        source::SourceRange{start, semicolon->range.end}, name->value,
        name->range, std::move(parameters), std::move(results), isUnsafe,
        std::move(borrowSource), borrowSourceRange);
}

std::unique_ptr<FunctionDeclaration> Parser::ParseFunctionDeclaration() {
    const auto tokenStart = _position;
    std::vector<std::pair<std::size_t, std::size_t>> erased;
    auto parametersAtName = [&](Parser& parser) {
        const auto begin = _position;
        auto parameters = parser.ParseGenericParameters();
        if (_position != begin)
            erased.emplace_back(begin - tokenStart, _position - tokenStart);
        return parameters;
    };
    const source::SourceLocation start = Current().range.start;
    std::vector<FunctionModifier> modifiers;
    while (Check(TokenType::Unsafe) || Check(TokenType::Private) ||
           Check(TokenType::Extern)) {
        const TokenType modifierToken = Advance().type;
        const auto modifier =
            modifierToken == TokenType::Unsafe    ? FunctionModifier::Unsafe
            : modifierToken == TokenType::Private ? FunctionModifier::Private
                                                  : FunctionModifier::Extern;
        if (std::find(modifiers.begin(), modifiers.end(), modifier) !=
            modifiers.end())
            ReportUnexpectedToken("after duplicate function modifier");
        modifiers.push_back(modifier);
    }
    std::unique_ptr<TypeNode> returnType = ParseType(true, false, true);
    if (returnType == nullptr) {
        return nullptr;
    }

    const Token* name = Consume(TokenType::Identifier, "function name");
    if (name == nullptr) {
        return nullptr;
    }
    const auto ownerToken =
        static_cast<std::size_t>(name - _tokens.data()) - tokenStart;
    auto genericParameters = parametersAtName(*this);
    std::vector<std::string> ownerParameters;
    std::string owner;
    if (Match(TokenType::Scope)) {
        ownerParameters = std::move(genericParameters);
        owner = name->value;
        name = Consume(TokenType::Identifier, "method name after '::'");
        if (name == nullptr) return nullptr;
        genericParameters = parametersAtName(*this);
    }
    if (Consume(TokenType::LParen, "'(' after function name") == nullptr) {
        return nullptr;
    }

    std::vector<std::unique_ptr<Parameter>> parameters;
    while (!Check(TokenType::RParen) && !IsAtEnd()) {
        std::unique_ptr<Parameter> parameter;
        // Receiver shorthand is only legal in the first slot of a method.
        if (!owner.empty() && parameters.empty() &&
            (Check(TokenType::Ampersand) || Check(TokenType::Mul)) &&
            ((Peek(1).type == TokenType::Identifier &&
              Peek(1).value == "self") ||
             (Peek(1).type == TokenType::Const &&
              Peek(2).type == TokenType::Identifier &&
              Peek(2).value == "self"))) {
            const Token& prefix = Advance();
            const bool readonly = Match(TokenType::Const);
            const Token& self = Advance();
            auto target =
                std::make_unique<TypeNode>(self.range, owner, readonly);
            auto receiver = std::make_unique<TypeNode>(
                source::SourceRange{prefix.range.start, self.range.end},
                prefix.type == TokenType::Mul ? TypeKind::Pointer
                                              : TypeKind::Reference,
                std::move(target));
            parameter = std::make_unique<Parameter>(
                receiver->Range(), std::move(receiver), self.value, self.range);
        } else
            parameter = ParseParameter();
        if (parameter != nullptr) {
            parameters.push_back(std::move(parameter));
        } else {
            while (!IsAtEnd() && !Check(TokenType::Comma) &&
                   !Check(TokenType::RParen)) {
                Advance();
            }
        }

        if (!Match(TokenType::Comma)) {
            break;
        }
    }

    if (Consume(TokenType::RParen, "')' after parameters") == nullptr) {
        return nullptr;
    }

    std::string borrowSource;
    source::SourceRange borrowSourceRange;
    if (Match(TokenType::LeftArrow)) {
        const Token* source =
            Consume(TokenType::Identifier, "borrow source after '<-'");
        if (source == nullptr) return nullptr;
        borrowSource = source->value;
        borrowSourceRange = source->range;
    }

    if (Match(TokenType::Override))
        modifiers.push_back(FunctionModifier::Override);
    std::unique_ptr<BlockStatement> body;
    source::SourceLocation end;
    if (std::find(modifiers.begin(), modifiers.end(),
                  FunctionModifier::Extern) != modifiers.end()) {
        const Token* semicolon =
            Consume(TokenType::SemiColon, "';' after extern declaration");
        if (semicolon == nullptr) return nullptr;
        end = semicolon->range.end;
        body = std::make_unique<BlockStatement>(
            semicolon->range, std::vector<std::unique_ptr<Statement>>{});
    } else {
        body = ParseBlockStatement();
        if (body == nullptr) return nullptr;
        end = body->Range().end;
    }

    auto declaration = std::make_unique<FunctionDeclaration>(
        source::SourceRange{start, end}, std::move(modifiers),
        std::move(returnType), name->value, std::move(parameters),
        std::move(body), name->range, std::move(owner), std::move(borrowSource),
        borrowSourceRange);
    declaration->genericParameters = std::move(genericParameters);
    declaration->ownerParameters = std::move(ownerParameters);
    declaration->parameterTokenRanges = std::move(erased);
    declaration->nameTokenIndex =
        static_cast<std::size_t>(name - _tokens.data()) - tokenStart;
    declaration->ownerTokenIndex = ownerToken;
    declaration->templateTokens.assign(_tokens.begin() + tokenStart,
                                       _tokens.begin() + _position);
    return declaration;
}

std::unique_ptr<ErrorDeclaration> Parser::ParseErrorDeclaration() {
    const auto start = Advance().range.start;
    const Token* name = Consume(TokenType::Identifier, "error type name");
    if (name == nullptr ||
        Consume(TokenType::LCurly, "'{' after error type name") == nullptr)
        return nullptr;
    std::vector<ErrorVariant> variants;
    if (!Check(TokenType::RCurly)) {
        do {
            const Token* variant =
                Consume(TokenType::Identifier, "error variant name");
            if (variant == nullptr) return nullptr;
            variants.push_back({variant->value, variant->range});
        } while (Match(TokenType::Comma) && !Check(TokenType::RCurly));
    }
    const Token* close = Consume(TokenType::RCurly, "'}' after error variants");
    if (close == nullptr) return nullptr;
    return std::make_unique<ErrorDeclaration>(
        source::SourceRange{start, close->range.end}, name->value, name->range,
        std::move(variants));
}

std::unique_ptr<EnumDeclaration> Parser::ParseEnumDeclaration() {
    const source::SourceLocation start = Advance().range.start;
    const Token* name = Consume(TokenType::Identifier, "enum type name");
    if (name == nullptr) return nullptr;

    std::unique_ptr<TypeNode> backingType;
    if (Match(TokenType::Colon)) {
        backingType = ParseType();
        if (backingType == nullptr) return nullptr;
    }
    if (Consume(TokenType::LCurly, "'{' after enum type name") == nullptr)
        return nullptr;

    std::vector<EnumVariant> variants;
    while (!Check(TokenType::RCurly) && !IsAtEnd()) {
        bool isDefault = false;
        if (Check(TokenType::Identifier) && Current().value == "default" &&
            Peek(1).type == TokenType::Identifier) {
            Advance();
            isDefault = true;
        }
        const Token* variant =
            Consume(TokenType::Identifier, "enum variant name");
        if (variant == nullptr) return nullptr;

        std::string discriminant;
        source::SourceRange discriminantRange;
        if (Match(TokenType::Assign)) {
            const Token* minus = nullptr;
            if (Check(TokenType::Sub)) minus = &Advance();
            const Token* value =
                Consume(TokenType::NumberLiteral, "integer discriminant");
            if (value == nullptr) return nullptr;
            discriminant = minus ? "-" + value->value : value->value;
            discriminantRange = minus ? source::SourceRange{minus->range.start,
                                                            value->range.end}
                                      : value->range;
        }
        variants.push_back({variant->value, variant->range, isDefault,
                            std::move(discriminant), discriminantRange});
        if (!Match(TokenType::Comma)) break;
    }
    const Token* close = Consume(TokenType::RCurly, "'}' after enum variants");
    if (close == nullptr) return nullptr;
    return std::make_unique<EnumDeclaration>(
        source::SourceRange{start, close->range.end}, name->value, name->range,
        std::move(backingType), std::move(variants));
}

std::unique_ptr<StructureDeclaration> Parser::ParseStructureDeclaration() {
    const auto tokenStart = _position;
    const auto start = Advance().range.start;
    const Token* name = Consume(TokenType::Identifier, "structure name");
    const auto parameterStart = _position;
    auto genericParameters = ParseGenericParameters();
    const auto parameterEnd = _position;
    if (!name || !Consume(TokenType::LCurly, "'{' after structure name"))
        return nullptr;
    std::vector<std::unique_ptr<StructureField>> fields;
    bool privateBlock = false;
    while (!IsAtEnd()) {
        if (Match(TokenType::RCurly)) {
            if (privateBlock) {
                privateBlock = false;
                continue;
            }
            const Token* end = Consume(TokenType::SemiColon,
                                       "';' after structure declaration");
            if (!end) return nullptr;
            auto declaration = std::make_unique<StructureDeclaration>(
                source::SourceRange{start, end->range.end}, name->value,
                name->range, std::move(fields));
            declaration->genericParameters = std::move(genericParameters);
            if (parameterStart != parameterEnd)
                declaration->parameterTokenRanges.emplace_back(
                    parameterStart - tokenStart, parameterEnd - tokenStart);
            declaration->nameTokenIndex =
                static_cast<std::size_t>(name - _tokens.data()) - tokenStart;
            declaration->templateTokens.assign(_tokens.begin() + tokenStart,
                                               _tokens.begin() + _position);
            return declaration;
        }
        bool isPrivate = privateBlock;
        if (Match(TokenType::Private)) {
            isPrivate = true;
            if (Match(TokenType::LCurly)) {
                if (privateBlock) {
                    ReportUnexpectedToken("in nested private block");
                    return nullptr;
                }
                privateBlock = true;
                continue;
            }
        }
        auto type = ParseType(true);
        if (!type) return nullptr;
        const auto fieldStart = type->Range().start;
        const Token* field = Consume(TokenType::Identifier, "field name");
        if (!field) return nullptr;
        std::unique_ptr<Expression> initializer;
        if (Match(TokenType::Assign)) {
            initializer = ParseExpression();
            if (!initializer) return nullptr;
        }
        const Token* end =
            Consume(TokenType::SemiColon, "';' after structure field");
        if (!end) return nullptr;
        fields.push_back(std::make_unique<StructureField>(
            source::SourceRange{fieldStart, end->range.end}, std::move(type),
            field->value, field->range, std::move(initializer), isPrivate));
    }
    ReportUnexpectedToken("in structure declaration");
    return nullptr;
}

std::unique_ptr<Declaration> Parser::ParseDeclaration() {
    if (Match(TokenType::Export)) {
        auto declaration = ParseDeclaration();
        if (declaration) {
            if (declaration->exported)
                _diagnostics.Report(
                    diagnostics::DiagnosticCode::UnexpectedToken,
                    declaration->Range(), "duplicate export modifier");
            declaration->exported = true;
        }
        return declaration;
    }
    if (Check(TokenType::Test)) return ParseTestDeclaration();
    if (Check(TokenType::Structure)) return ParseStructureDeclaration();
    if (Check(TokenType::Enum)) return ParseEnumDeclaration();
    if (Check(TokenType::Error) && Peek(1).type == TokenType::Identifier &&
        Peek(2).type == TokenType::LCurly)
        return ParseErrorDeclaration();
    if (Check(TokenType::Func) ||
        (Check(TokenType::Unsafe) && Peek(1).type == TokenType::Func))
        return ParseCallableTypeDeclaration();
    if (StartsType() || Check(TokenType::Unsafe) || Check(TokenType::Private) ||
        Check(TokenType::Extern)) {
        return ParseFunctionDeclaration();
    }

    ReportUnexpectedToken("at top level");
    return nullptr;
}

std::unique_ptr<FunctionDeclaration> Parser::ParseTestDeclaration() {
    const Token& keyword = Advance();
    const Token* name = Consume(TokenType::Identifier, "test name");
    if (name == nullptr ||
        Consume(TokenType::LParen, "'(' after test name") == nullptr)
        return nullptr;

    std::vector<std::unique_ptr<Parameter>> parameters;
    while (!Check(TokenType::RParen) && !IsAtEnd()) {
        auto parameter = ParseParameter();
        if (parameter != nullptr) parameters.push_back(std::move(parameter));
        if (!Match(TokenType::Comma)) break;
    }
    if (Consume(TokenType::RParen, "')' after test parameters") == nullptr)
        return nullptr;
    auto body = ParseBlockStatement();
    if (body == nullptr) return nullptr;

    auto declaration = std::make_unique<FunctionDeclaration>(
        source::SourceRange{keyword.range.start, body->Range().end},
        std::vector<FunctionModifier>{},
        std::make_unique<TypeNode>(keyword.range, PrimitiveType::Void),
        name->value, std::move(parameters), std::move(body), name->range);
    declaration->MarkTest();
    return declaration;
}

std::unique_ptr<BlockStatement> Parser::ParseBlockStatement(bool isUnsafe) {
    const source::SourceLocation start = Current().range.start;
    if (isUnsafe) Advance();
    const Token* openBrace = Consume(TokenType::LCurly, "'{' to start block");
    if (openBrace == nullptr) {
        return nullptr;
    }

    std::vector<std::unique_ptr<Statement>> statements;
    while (!Check(TokenType::RCurly) && !IsAtEnd()) {
        std::unique_ptr<Statement> statement = ParseStatement();
        if (statement != nullptr) {
            statements.push_back(std::move(statement));
        } else {
            SynchronizeStatement();
        }
    }

    const Token* closeBrace = Consume(TokenType::RCurly, "'}' after block");
    const source::SourceLocation end =
        closeBrace == nullptr ? Current().range.end : closeBrace->range.end;
    return std::make_unique<BlockStatement>(source::SourceRange{start, end},
                                            std::move(statements), isUnsafe);
}

std::unique_ptr<Statement> Parser::ParseStatement() {
    if (Check(TokenType::LCurly)) return ParseBlockStatement();
    if (Check(TokenType::Unsafe)) return ParseBlockStatement(true);
    if (Check(TokenType::Defer)) {
        const Token& keyword = Advance();
        auto call = ParseExpression();
        if (call == nullptr) return nullptr;
        const Token* end =
            Consume(TokenType::SemiColon, "';' after deferred call");
        const source::SourceRange range{
            keyword.range.start,
            end == nullptr ? call->Range().end : end->range.end};
        return std::make_unique<DeferStatement>(range, std::move(call));
    }
    if (Check(TokenType::Return)) {
        return ParseReturnStatement();
    }
    if (Check(TokenType::If)) {
        return ParseIfStatement();
    }
    if (Check(TokenType::While)) {
        return ParseWhileStatement();
    }
    if (Check(TokenType::Loop)) return ParseLoopStatement();
    if (Check(TokenType::For)) return ParseRangeStatement();
    if (Check(TokenType::Proceed)) return ParseProceedStatement();
    if (Check(TokenType::Break)) return ParseBreakStatement();
    if (Check(TokenType::JumpTo)) return ParseJumpToStatement();
    if (Check(TokenType::Hash)) return ParsePositionStatement();
    if (IsDestructureStatement()) return ParseDestructureStatement();
    if (StartsType() || IsInferredVariableDeclaration()) {
        return ParseVariableDeclarationStatement();
    }

    const source::SourceLocation start = Current().range.start;
    std::unique_ptr<Expression> expression = ParseExpression();
    if (expression == nullptr) {
        return nullptr;
    }
    const Token* semicolon =
        Consume(TokenType::SemiColon, "';' after expression");
    const source::SourceLocation end =
        semicolon == nullptr ? expression->Range().end : semicolon->range.end;
    return std::make_unique<ExpressionStatement>(
        source::SourceRange{start, end}, std::move(expression));
}

bool Parser::HasParenthesizedBindings() const {
    if (!Check(TokenType::LParen)) return false;
    std::size_t depth = 0;
    bool comma = false;
    for (std::size_t offset = 0;; ++offset) {
        const TokenType token = Peek(offset).type;
        if (token == TokenType::EOF_TOKEN || token == TokenType::SemiColon ||
            token == TokenType::LCurly)
            return false;
        if (token == TokenType::Comma && depth == 1) comma = true;
        if (token == TokenType::LParen) ++depth;
        if (token == TokenType::RParen && --depth == 0)
            return comma && (Peek(offset + 1).type == TokenType::Assign ||
                             Peek(offset + 1).type == TokenType::DeclareAssign);
    }
}

bool Parser::IsDestructureStatement() const {
    if (HasParenthesizedBindings()) return true;
    std::size_t parentheses = 0;
    std::size_t brackets = 0;
    bool comma = false;
    for (std::size_t offset = 0;; ++offset) {
        const TokenType token = Peek(offset).type;
        if (token == TokenType::EOF_TOKEN || token == TokenType::SemiColon ||
            token == TokenType::LCurly || token == TokenType::RCurly)
            return false;
        if (parentheses == 0 && brackets == 0) {
            if (token == TokenType::Assign || token == TokenType::DeclareAssign)
                return comma;
            if (token == TokenType::Comma) comma = true;
        }
        if (token == TokenType::LParen) ++parentheses;
        if (token == TokenType::RParen) {
            if (parentheses == 0) return false;
            --parentheses;
        }
        if (token == TokenType::LBracket) ++brackets;
        if (token == TokenType::RBracket) {
            if (brackets == 0) return false;
            --brackets;
        }
    }
}

std::unique_ptr<Statement> Parser::ParseDestructureStatement() {
    const source::SourceLocation start = Current().range.start;
    const bool parenthesized = HasParenthesizedBindings();
    if (parenthesized) Advance();
    std::vector<std::unique_ptr<DestructureBinding>> bindings;
    std::unique_ptr<TypeNode> sharedType;
    do {
        std::unique_ptr<TypeNode> type;
        if (StartsType()) {
            type = ParseType();
            if (type == nullptr) return nullptr;
            if (!parenthesized) sharedType = CloneType(*type);
        } else if (!parenthesized && sharedType != nullptr) {
            type = CloneType(*sharedType);
        }
        const Token* name =
            Consume(TokenType::Identifier, "destructuring binding name or '_'");
        if (name == nullptr) return nullptr;
        bindings.push_back(std::make_unique<DestructureBinding>(
            name->range, name->value, std::move(type)));
        if (!Match(TokenType::Comma)) break;
        if (parenthesized && Check(TokenType::RParen)) break;
    } while (true);
    if (parenthesized && Consume(TokenType::RParen,
                                 "')' after destructuring bindings") == nullptr)
        return nullptr;
    const bool inferred = Match(TokenType::DeclareAssign);
    if (!inferred && Consume(TokenType::Assign,
                             "'=' after destructuring bindings") == nullptr)
        return nullptr;
    auto value = ParseExpression();
    if (value == nullptr) return nullptr;
    const Token* end = Consume(TokenType::SemiColon, "';' after destructuring");
    return std::make_unique<DestructureStatement>(
        source::SourceRange{
            start, end == nullptr ? value->Range().end : end->range.end},
        std::move(bindings), std::move(value), inferred);
}

std::unique_ptr<VariableDeclarationStatement>
Parser::ParseVariableDeclarationStatement() {
    const source::SourceLocation start = Current().range.start;
    std::unique_ptr<TypeNode> type;
    const Token* name = nullptr;
    bool hasInitializer = false;
    if (IsInferredVariableDeclaration()) {
        name = &Advance();
        Advance();
        hasInitializer = true;
    } else {
        type = ParseType();
        if (type == nullptr) {
            return nullptr;
        }

        name = Consume(TokenType::Identifier, "variable name");
        if (name == nullptr) {
            return nullptr;
        }
        if (Check(TokenType::Assign)) {
            Advance();
            hasInitializer = true;
        }
    }

    std::unique_ptr<Expression> initializer;
    if (hasInitializer) {
        initializer = ParseExpression();
        if (initializer == nullptr) return nullptr;
    }

    const Token* semicolon =
        Consume(TokenType::SemiColon, "';' after variable declaration");
    const source::SourceLocation end =
        semicolon == nullptr
            ? (initializer == nullptr ? name->range.end
                                      : initializer->Range().end)
            : semicolon->range.end;
    return std::make_unique<VariableDeclarationStatement>(
        source::SourceRange{start, end}, std::move(type), name->value,
        std::move(initializer), name->range);
}

std::unique_ptr<AlsoClause> Parser::ParseAlsoClause() {
    const Token& alsoToken = Advance();
    if (Check(TokenType::LCurly)) {
        _diagnostics.Report(diagnostics::DiagnosticCode::ExpectedToken,
                            Current().range,
                            "expected a condition after 'also'");
        ParseBlockStatement();
        return nullptr;
    }

    std::unique_ptr<Expression> condition = ParseExpression();
    if (condition == nullptr) {
        return nullptr;
    }
    std::unique_ptr<BlockStatement> body = ParseBlockStatement();
    if (body == nullptr) {
        return nullptr;
    }
    const source::SourceLocation end = body->Range().end;
    return std::make_unique<AlsoClause>(
        source::SourceRange{alsoToken.range.start, end}, std::move(condition),
        std::move(body));
}

std::unique_ptr<ConditionalBranch> Parser::ParseConditionalBranch(
    source::SourceLocation start) {
    std::unique_ptr<Expression> condition = ParseExpression();
    if (condition == nullptr) {
        return nullptr;
    }
    std::unique_ptr<BlockStatement> body = ParseBlockStatement();
    if (body == nullptr) {
        return nullptr;
    }

    source::SourceLocation end = body->Range().end;
    std::unique_ptr<AlsoClause> alsoClause;
    if (Check(TokenType::Also)) {
        alsoClause = ParseAlsoClause();
        if (alsoClause != nullptr) {
            end = alsoClause->Range().end;
        }
    }

    while (Check(TokenType::Also)) {
        _diagnostics.Report(diagnostics::DiagnosticCode::UnexpectedToken,
                            Current().range,
                            "a branch may have only one 'also' clause");
        std::unique_ptr<AlsoClause> extraClause = ParseAlsoClause();
        if (extraClause != nullptr) {
            end = extraClause->Range().end;
        }
    }

    return std::make_unique<ConditionalBranch>(
        source::SourceRange{start, end}, std::move(condition), std::move(body),
        std::move(alsoClause));
}

std::unique_ptr<IfStatement> Parser::ParseIfStatement() {
    const Token& ifToken = Advance();
    std::unique_ptr<ConditionalBranch> primaryBranch =
        ParseConditionalBranch(ifToken.range.start);
    if (primaryBranch == nullptr) {
        return nullptr;
    }

    source::SourceLocation end = primaryBranch->Range().end;
    std::vector<std::unique_ptr<ConditionalBranch>> elseIfBranches;
    std::unique_ptr<BlockStatement> elseBody;

    while (Check(TokenType::Else)) {
        const Token& elseToken = Advance();
        if (Match(TokenType::If)) {
            std::unique_ptr<ConditionalBranch> branch =
                ParseConditionalBranch(elseToken.range.start);
            if (branch == nullptr) {
                return nullptr;
            }
            end = branch->Range().end;
            elseIfBranches.push_back(std::move(branch));
            continue;
        }

        elseBody = ParseBlockStatement();
        if (elseBody != nullptr) {
            end = elseBody->Range().end;
        }
        break;
    }

    return std::make_unique<IfStatement>(
        source::SourceRange{ifToken.range.start, end}, std::move(primaryBranch),
        std::move(elseIfBranches), std::move(elseBody));
}

std::unique_ptr<WhileStatement> Parser::ParseWhileStatement() {
    const Token& whileToken = Advance();
    std::unique_ptr<Expression> condition = ParseExpression();
    if (condition == nullptr) return nullptr;

    std::unique_ptr<BlockStatement> body = ParseBlockStatement();
    if (body == nullptr) return nullptr;

    source::SourceLocation end = body->Range().end;
    std::unique_ptr<AlsoClause> alsoClause;
    if (Check(TokenType::Also)) {
        alsoClause = ParseAlsoClause();
        if (alsoClause != nullptr) end = alsoClause->Range().end;
    }

    while (Check(TokenType::Also)) {
        _diagnostics.Report(diagnostics::DiagnosticCode::UnexpectedToken,
                            Current().range,
                            "a while loop may have only one 'also' clause");
        std::unique_ptr<AlsoClause> extraClause = ParseAlsoClause();
        if (extraClause != nullptr) end = extraClause->Range().end;
    }

    return std::make_unique<WhileStatement>(
        source::SourceRange{whileToken.range.start, end}, std::move(condition),
        std::move(body), std::move(alsoClause));
}

std::unique_ptr<Statement> Parser::ParseLoopStatement() {
    const Token& keyword = Advance();
    std::vector<std::unique_ptr<Statement>> statements;
    if (!Match(TokenType::SemiColon)) {
        if (StartsType() || IsInferredVariableDeclaration()) {
            auto initializer = ParseVariableDeclarationStatement();
            if (initializer == nullptr) return nullptr;
            statements.push_back(std::move(initializer));
        } else {
            auto initializer = ParseExpression();
            if (initializer == nullptr) return nullptr;
            const source::SourceRange range = initializer->Range();
            statements.push_back(std::make_unique<ExpressionStatement>(
                range, std::move(initializer)));
            if (Consume(TokenType::SemiColon, "';' after loop initializer") ==
                nullptr)
                return nullptr;
        }
    }
    std::unique_ptr<Expression> condition;
    if (Check(TokenType::SemiColon))
        condition = std::make_unique<LiteralExpression>(
            Current().range, TokenType::True, "true");
    else
        condition = ParseExpression();
    if (condition == nullptr ||
        Consume(TokenType::SemiColon, "';' after loop condition") == nullptr)
        return nullptr;
    std::unique_ptr<Expression> update;
    if (!Check(TokenType::LCurly)) {
        update = ParseExpression();
        if (update == nullptr) return nullptr;
    }
    auto body = ParseBlockStatement();
    if (body == nullptr) return nullptr;
    const source::SourceRange range{keyword.range.start, body->Range().end};
    statements.push_back(std::make_unique<WhileStatement>(
        range, std::move(condition), std::move(body), nullptr,
        std::move(update)));
    return std::make_unique<BlockStatement>(range, std::move(statements));
}

std::unique_ptr<RangeStatement> Parser::ParseRangeStatement() {
    const Token& keyword = Advance();
    const bool pair = Match(TokenType::LParen);
    const bool typed = StartsType();
    std::vector<std::unique_ptr<TypeNode>> types;
    std::vector<const Token*> names;
    for (std::size_t binding = 0; binding < (pair ? 2u : 1u); ++binding) {
        types.push_back(typed ? ParseType() : nullptr);
        if (typed && types.back() == nullptr) return nullptr;
        const Token* name =
            Consume(TokenType::Identifier, "range binding name");
        if (name == nullptr) return nullptr;
        names.push_back(name);
        if (pair && binding == 0 &&
            Consume(TokenType::Comma, "',' between range bindings") == nullptr)
            return nullptr;
    }
    if (pair &&
        Consume(TokenType::RParen, "')' after range bindings") == nullptr)
        return nullptr;
    if (Consume(typed ? TokenType::Assign : TokenType::DeclareAssign,
                typed ? "'=' after typed range bindings"
                      : "':=' after inferred range bindings") == nullptr)
        return nullptr;
    if (Consume(TokenType::Range, "'range' before array expression") == nullptr)
        return nullptr;
    auto source = ParseExpression();
    if (source == nullptr) return nullptr;
    const source::SourceRange sourceRange = source->Range();
    auto body = ParseBlockStatement();
    if (body == nullptr) return nullptr;
    const source::SourceRange range{keyword.range.start, body->Range().end};
    // '$' cannot occur in source identifiers, so these names cannot collide.
    const std::string suffix = std::to_string(_rangeId++);
    const std::string sourceName = "$range" + suffix;
    const std::string cursorName = "$cursor" + suffix;
    const auto Name = [&](const std::string& name) {
        return std::make_unique<NameExpression>(sourceRange, name);
    };
    std::vector<std::unique_ptr<Statement>> outer;
    auto snapshot = std::make_unique<VariableDeclarationStatement>(
        sourceRange, nullptr, sourceName, std::move(source), sourceRange, true);
    const VariableDeclarationStatement* sourceDeclaration = snapshot.get();
    outer.push_back(std::move(snapshot));
    outer.push_back(std::make_unique<VariableDeclarationStatement>(
        range, std::make_unique<TypeNode>(range, PrimitiveType::USize),
        cursorName,
        std::make_unique<LiteralExpression>(range, TokenType::NumberLiteral,
                                            "0"),
        range));
    std::vector<std::unique_ptr<Statement>> iteration;
    const VariableDeclarationStatement* indexBinding = nullptr;
    for (std::size_t binding = 0; binding < names.size(); ++binding) {
        std::unique_ptr<Expression> value;
        if (binding == 0) {
            value = Name(cursorName);
            // A typed index uses a checked-at-analysis conversion. The cursor
            // itself remains usize even when the exposed index is narrower.
            if (types[binding] != nullptr) {
                value = std::make_unique<CastExpression>(
                    names[binding]->range, std::move(value),
                    CloneType(*types[binding]));
            }
        } else {
            value = std::make_unique<IndexExpression>(
                sourceRange, Name(sourceName), Name(cursorName), true);
        }
        auto declaration = std::make_unique<VariableDeclarationStatement>(
            names[binding]->range, std::move(types[binding]),
            names[binding]->value == "_"
                ? "$discard" + suffix + "." + std::to_string(binding)
                : names[binding]->value,
            std::move(value), names[binding]->range);
        if (binding == 0) indexBinding = declaration.get();
        iteration.push_back(std::move(declaration));
    }
    iteration.push_back(std::move(body));
    auto condition = std::make_unique<BinaryExpression>(
        range, Name(cursorName), TokenType::Less,
        std::make_unique<MemberCallExpression>(
            sourceRange, Name(sourceName), "len", sourceRange,
            std::vector<std::unique_ptr<Expression>>{}));
    auto update = std::make_unique<UnaryExpression>(range, TokenType::Increment,
                                                    Name(cursorName));
    outer.push_back(std::make_unique<WhileStatement>(
        range, std::move(condition),
        std::make_unique<BlockStatement>(range, std::move(iteration)), nullptr,
        std::move(update)));
    return std::make_unique<RangeStatement>(
        range, std::make_unique<BlockStatement>(range, std::move(outer)),
        sourceDeclaration, indexBinding);
}

std::unique_ptr<ProceedStatement> Parser::ParseProceedStatement() {
    const Token& keyword = Advance();
    const Token* semicolon =
        Consume(TokenType::SemiColon, "';' after 'proceed'");
    const source::SourceLocation end =
        semicolon == nullptr ? keyword.range.end : semicolon->range.end;
    return std::make_unique<ProceedStatement>(
        source::SourceRange{keyword.range.start, end});
}

std::unique_ptr<BreakStatement> Parser::ParseBreakStatement() {
    const Token& keyword = Advance();
    const Token* semicolon = Consume(TokenType::SemiColon, "';' after 'break'");
    const source::SourceLocation end =
        semicolon == nullptr ? keyword.range.end : semicolon->range.end;
    return std::make_unique<BreakStatement>(
        source::SourceRange{keyword.range.start, end});
}

std::unique_ptr<JumpToStatement> Parser::ParseJumpToStatement() {
    const Token& keyword = Advance();
    const Token* position = Consume(TokenType::Identifier, "position name");
    if (position == nullptr) return nullptr;
    const Token* semicolon =
        Consume(TokenType::SemiColon, "';' after position name");
    const source::SourceLocation end =
        semicolon == nullptr ? position->range.end : semicolon->range.end;
    return std::make_unique<JumpToStatement>(
        source::SourceRange{keyword.range.start, end}, position->value,
        position->range);
}

std::unique_ptr<PositionStatement> Parser::ParsePositionStatement() {
    const Token& hash = Advance();
    const Token* name =
        Consume(TokenType::Identifier, "position name after '#'");
    if (name == nullptr) return nullptr;
    return std::make_unique<PositionStatement>(
        source::SourceRange{hash.range.start, name->range.end}, name->value,
        name->range);
}

std::unique_ptr<ReturnStatement> Parser::ParseReturnStatement() {
    const Token& returnToken = Advance();
    std::unique_ptr<Expression> expression;
    if (!Check(TokenType::SemiColon)) {
        expression = ParseExpression();
    }

    const Token* semicolon =
        Consume(TokenType::SemiColon, "';' after return statement");
    const source::SourceLocation end =
        semicolon == nullptr ? Current().range.start : semicolon->range.end;
    return std::make_unique<ReturnStatement>(
        source::SourceRange{returnToken.range.start, end},
        std::move(expression));
}

std::unique_ptr<Expression> Parser::ParseExpression() {
    return ParseAssignmentExpression();
}

std::unique_ptr<Expression> Parser::ParseAssignmentExpression() {
    std::unique_ptr<Expression> target = ParseBinaryExpression(1);
    if (target == nullptr || !Match(TokenType::Assign)) {
        return target;
    }

    const source::SourceRange targetRange = target->Range();
    const Expression* place = target.get();
    while (place->Kind() == AstNodeKind::ParenthesizedExpression) {
        place = &static_cast<const ParenthesizedExpression&>(*place).Inner();
    }
    const bool dereference =
        place->Kind() == AstNodeKind::UnaryExpression &&
        static_cast<const UnaryExpression&>(*place).OperatorType() ==
            TokenType::Mul;
    if (place->Kind() != AstNodeKind::NameExpression &&
        place->Kind() != AstNodeKind::MemberAccessExpression &&
        place->Kind() != AstNodeKind::IndexExpression &&
        place->Kind() != AstNodeKind::MemberCallExpression && !dereference) {
        _diagnostics.Report(
            diagnostics::DiagnosticCode::InvalidAssignmentTarget, targetRange,
            "expression is not a valid assignment target");
    }

    std::unique_ptr<Expression> value = ParseAssignmentExpression();
    if (value == nullptr) {
        return target;
    }

    return std::make_unique<AssignmentExpression>(
        source::SourceRange{targetRange.start, value->Range().end},
        std::move(target), std::move(value));
}

int Parser::BinaryPrecedence(TokenType type) const {
    switch (type) {
        case TokenType::Or:
            return 1;
        case TokenType::And:
            return 2;
        case TokenType::Xor:
            return 3;
        case TokenType::Equal:
        case TokenType::NotEqual:
        case TokenType::StrictEqual:
        case TokenType::StrictNotEqual:
            return 4;
        case TokenType::Less:
        case TokenType::LessEqual:
        case TokenType::Greater:
        case TokenType::GreaterEqual:
            return 5;
        case TokenType::Add:
        case TokenType::Sub:
            return 6;
        case TokenType::Mul:
        case TokenType::Div:
        case TokenType::Mod:
            return 7;
        default:
            return 0;
    }
}

int Parser::UnaryPrecedence(TokenType type) const {
    switch (type) {
        case TokenType::Add:
        case TokenType::Sub:
        case TokenType::Bang:
        case TokenType::Mul:
        case TokenType::Ampersand:
            return 8;
        default:
            return 0;
    }
}

std::unique_ptr<Expression> Parser::ParseBinaryExpression(
    int minimumPrecedence) {
    std::unique_ptr<Expression> left = ParseUnaryExpression();
    if (left == nullptr) {
        return nullptr;
    }

    while (true) {
        const int precedence = BinaryPrecedence(Current().type);
        if (precedence < minimumPrecedence) {
            break;
        }

        const Token& operatorToken = Advance();
        std::unique_ptr<Expression> right =
            ParseBinaryExpression(precedence + 1);
        if (right == nullptr) {
            return left;
        }

        const source::SourceRange range{left->Range().start,
                                        right->Range().end};
        left = std::make_unique<BinaryExpression>(
            range, std::move(left), operatorToken.type, std::move(right));
    }

    return left;
}

std::unique_ptr<Expression> Parser::ParseUnaryExpression() {
    if (UnaryPrecedence(Current().type) == 0) {
        return ParsePostfixExpression();
    }

    const Token& operatorToken = Advance();
    TokenType operation = operatorToken.type;
    if (operation == TokenType::Ampersand && Check(TokenType::Identifier) &&
        Current().value == "raw") {
        Advance();
        operation = TokenType::RawAddress;
    }
    std::unique_ptr<Expression> operand = ParseUnaryExpression();
    if (operand == nullptr) {
        return nullptr;
    }

    return std::make_unique<UnaryExpression>(
        source::SourceRange{operatorToken.range.start, operand->Range().end},
        operation, std::move(operand));
}

std::unique_ptr<Expression> Parser::ParsePostfixExpression() {
    std::unique_ptr<Expression> expression = ParsePrimaryExpression();
    if (expression == nullptr) {
        return nullptr;
    }

    while (true) {
        if (expression->Kind() == AstNodeKind::NameExpression &&
            static_cast<const NameExpression&>(*expression).Name() == "new" &&
            Match(TokenType::Less)) {
            auto type = ParseType();
            if (!type ||
                !Consume(TokenType::Greater, "'>' after allocation type") ||
                !Consume(TokenType::LParen, "'(' after new<T>"))
                return expression;
            source::SourceLocation end = expression->Range().end;
            auto arguments = ParseArgumentList(end);
            expression = std::make_unique<CallExpression>(
                source::SourceRange{expression->Range().start, end},
                std::move(expression), std::move(arguments), std::move(type));
            continue;
        }
        if (expression->Kind() == AstNodeKind::NameExpression &&
            Check(TokenType::Less)) {
            std::size_t offset = 0;
            if (ScanTypeArguments(offset) &&
                (Peek(offset).type == TokenType::LParen ||
                 Peek(offset).type == TokenType::Scope)) {
                static_cast<NameExpression&>(*expression).typeArguments =
                    ParseTypeArguments();
                continue;
            }
        }
        if (Check(TokenType::Increment)) {
            const Token& operation = Advance();
            expression = std::make_unique<UnaryExpression>(
                source::SourceRange{expression->Range().start,
                                    operation.range.end},
                TokenType::Increment, std::move(expression));
            continue;
        }
        if (Match(TokenType::As)) {
            std::unique_ptr<TypeNode> targetType = ParseType();
            if (targetType == nullptr) return expression;
            const source::SourceLocation end = targetType->Range().end;
            expression = std::make_unique<CastExpression>(
                source::SourceRange{expression->Range().start, end},
                std::move(expression), std::move(targetType));
            continue;
        }

        if (Check(TokenType::Dot) || Check(TokenType::Arrow) ||
            Check(TokenType::Scope)) {
            const TokenType access = Advance().type;
            const Token* member = nullptr;
            if (Check(TokenType::Identifier) || Check(TokenType::String)) {
                member = &Advance();
            } else {
                member =
                    Consume(TokenType::Identifier, "member name after '.'");
            }
            if (member == nullptr) return expression;
            if (member->value == "cast" && Match(TokenType::Less)) {
                std::unique_ptr<TypeNode> targetType = ParseType();
                if (targetType == nullptr) return expression;
                const Token* close =
                    Consume(TokenType::Greater, "'>' after cast type");
                const source::SourceLocation end = close == nullptr
                                                       ? targetType->Range().end
                                                       : close->range.end;
                expression = std::make_unique<CastExpression>(
                    source::SourceRange{expression->Range().start, end},
                    std::move(expression), std::move(targetType));
                continue;
            }
            std::vector<std::unique_ptr<TypeNode>> typeArguments;
            std::size_t argumentEnd = 0;
            if (Check(TokenType::Less) && ScanTypeArguments(argumentEnd) &&
                Peek(argumentEnd).type == TokenType::LParen)
                typeArguments = ParseTypeArguments();
            if (!Match(TokenType::LParen)) {
                expression = std::make_unique<MemberAccessExpression>(
                    source::SourceRange{expression->Range().start,
                                        member->range.end},
                    std::move(expression), member->value, member->range,
                    access);
                continue;
            }
            source::SourceLocation end = member->range.end;
            std::vector<std::unique_ptr<Expression>> arguments =
                ParseArgumentList(end);
            // Reference member spelling shares the dereference AST, including
            // its lvalue, borrowing, and code-generation behavior.
            if (access == TokenType::Arrow && member->value == "deref" &&
                typeArguments.empty() && arguments.empty()) {
                expression = std::make_unique<UnaryExpression>(
                    source::SourceRange{expression->Range().start, end},
                    TokenType::Mul, std::move(expression), true);
                continue;
            }
            expression = std::make_unique<MemberCallExpression>(
                source::SourceRange{expression->Range().start, end},
                std::move(expression), member->value, member->range,
                std::move(arguments), access);
            static_cast<MemberCallExpression&>(*expression).typeArguments =
                std::move(typeArguments);
            continue;
        }

        if (Match(TokenType::LBracket)) {
            std::unique_ptr<Expression> index;
            if (!Check(TokenType::Colon)) {
                index = ParseExpression();
                if (index == nullptr) return expression;
            }
            if (Match(TokenType::Colon)) {
                std::unique_ptr<Expression> upper;
                if (!Check(TokenType::RBracket)) {
                    upper = ParseExpression();
                    if (upper == nullptr) return expression;
                }
                const Token* close =
                    Consume(TokenType::RBracket, "']' after slice bounds");
                const source::SourceLocation end =
                    close == nullptr ? Current().range.start : close->range.end;
                expression = std::make_unique<SliceExpression>(
                    source::SourceRange{expression->Range().start, end},
                    std::move(expression), std::move(index), std::move(upper));
                continue;
            }
            const Token* close =
                Consume(TokenType::RBracket, "']' after index");
            const source::SourceLocation end =
                close == nullptr ? index->Range().end : close->range.end;
            expression = std::make_unique<IndexExpression>(
                source::SourceRange{expression->Range().start, end},
                std::move(expression), std::move(index));
            continue;
        }

        if (!Match(TokenType::LParen)) break;
        source::SourceLocation end = expression->Range().end;
        std::vector<std::unique_ptr<Expression>> arguments =
            ParseArgumentList(end);
        expression = std::make_unique<CallExpression>(
            source::SourceRange{expression->Range().start, end},
            std::move(expression), std::move(arguments));
    }

    return expression;
}

std::vector<std::unique_ptr<Expression>> Parser::ParseArgumentList(
    source::SourceLocation& end) {
    std::vector<std::unique_ptr<Expression>> arguments;
    while (!Check(TokenType::RParen) && !IsAtEnd()) {
        std::unique_ptr<Expression> argument = ParseExpression();
        if (argument == nullptr) {
            while (!IsAtEnd() && !Check(TokenType::Comma) &&
                   !Check(TokenType::RParen) && !Check(TokenType::SemiColon)) {
                Advance();
            }
        } else {
            end = argument->Range().end;
            arguments.push_back(std::move(argument));
        }

        if (Match(TokenType::Comma)) {
            if (Check(TokenType::RParen)) {
                _diagnostics.Report(diagnostics::DiagnosticCode::ExpectedToken,
                                    Current().range,
                                    "expected an argument after ','");
            }
            continue;
        }
        if (!Check(TokenType::RParen)) {
            _diagnostics.Report(diagnostics::DiagnosticCode::ExpectedToken,
                                Current().range,
                                "expected ',' or ')' after argument");
            while (!IsAtEnd() && !Check(TokenType::RParen) &&
                   !Check(TokenType::SemiColon)) {
                Advance();
            }
        }
        break;
    }

    const Token* closeParen = Consume(TokenType::RParen, "')' after arguments");
    if (closeParen != nullptr) end = closeParen->range.end;
    return arguments;
}

bool Parser::IsAnonymousFunctionStart() const {
    std::size_t offset = 0;
    if (Peek(offset).type == TokenType::Unsafe) ++offset;
    if (Peek(offset).type == TokenType::LBracket) {
        std::size_t depth = 0;
        do {
            const TokenType token = Peek(offset).type;
            if (token == TokenType::EOF_TOKEN) return false;
            if (token == TokenType::LBracket) ++depth;
            if (token == TokenType::RBracket && --depth == 0) {
                ++offset;
                break;
            }
            ++offset;
        } while (true);
    }
    if (Peek(offset).type != TokenType::LParen) return false;

    std::size_t depth = 0;
    do {
        const TokenType token = Peek(offset).type;
        if (token == TokenType::EOF_TOKEN) return false;
        if (token == TokenType::LParen) ++depth;
        if (token == TokenType::RParen && --depth == 0)
            return Peek(offset + 1).type == TokenType::FatArrow;
        ++offset;
    } while (true);
}

std::unique_ptr<Expression> Parser::ParseAnonymousFunctionExpression() {
    const Token& first = Current();
    const source::SourceLocation start = first.range.start;
    const bool isUnsafe = Match(TokenType::Unsafe);
    const bool explicitCaptureList = Match(TokenType::LBracket);
    std::vector<std::unique_ptr<AnonymousFunctionExpression::Capture>>
        captures;
    if (explicitCaptureList) {
        while (!Check(TokenType::RBracket) && !IsAtEnd()) {
            const Token* mode =
                Consume(TokenType::Identifier,
                        "'copy' before a captured name");
            if (mode == nullptr) return nullptr;
            if (mode->value != "copy") {
                _diagnostics.Report(
                    diagnostics::DiagnosticCode::UnexpectedToken, mode->range,
                    "expected 'copy' before a captured name");
                return nullptr;
            }
            const Token* name =
                Consume(TokenType::Identifier, "captured variable name");
            if (name == nullptr) return nullptr;
            captures.push_back(
                std::make_unique<AnonymousFunctionExpression::Capture>(
                    source::SourceRange{mode->range.start, name->range.end},
                    AnonymousFunctionExpression::CaptureMode::Copy,
                    name->value, name->range));
            if (!Match(TokenType::Comma)) break;
        }
        if (Consume(TokenType::RBracket,
                    "']' after anonymous function capture list") == nullptr)
            return nullptr;
    }
    if (Consume(TokenType::LParen,
                "'(' before anonymous function parameters") == nullptr)
        return nullptr;

    std::vector<std::unique_ptr<Parameter>> parameters;
    std::optional<bool> typedParameters;
    while (!Check(TokenType::RParen) && !IsAtEnd()) {
        std::size_t typeEnd = 0;
        const bool typed =
            ScanType(typeEnd) && Peek(typeEnd).type == TokenType::Identifier;
        if (typedParameters.has_value() && *typedParameters != typed)
            _diagnostics.Report(
                diagnostics::DiagnosticCode::UnexpectedToken, Current().range,
                "anonymous function parameters must be either all typed or "
                "all inferred");
        typedParameters = typedParameters.value_or(typed);

        const source::SourceLocation parameterStart = Current().range.start;
        std::unique_ptr<TypeNode> type;
        if (typed)
            type = ParseType(true, true, true);
        else
            type = std::make_unique<TypeNode>(Current().range,
                                              PrimitiveType::Void);
        if (type == nullptr) return nullptr;
        const Token* name =
            Consume(TokenType::Identifier, "anonymous function parameter name");
        if (name == nullptr) return nullptr;
        parameters.push_back(std::make_unique<Parameter>(
            source::SourceRange{parameterStart, name->range.end},
            std::move(type), name->value, name->range));
        if (!Match(TokenType::Comma)) break;
    }
    if (Consume(TokenType::RParen, "')' after anonymous function parameters") ==
            nullptr ||
        Consume(TokenType::FatArrow,
                "'=>' after anonymous function parameters") == nullptr)
        return nullptr;

    std::vector<std::unique_ptr<TypeNode>> results;
    source::SourceRange resultRange = Current().range;
    const bool inferredSignature = !Check(TokenType::LParen);
    if (!inferredSignature) {
        const Token& open = Advance();
        resultRange = open.range;
        while (!Check(TokenType::RParen) && !IsAtEnd()) {
            auto result = ParseType(true, false, true);
            if (result == nullptr) return nullptr;
            results.push_back(std::move(result));
            if (!Match(TokenType::Comma)) break;
        }
        const Token* close =
            Consume(TokenType::RParen, "')' after anonymous function results");
        if (close == nullptr) return nullptr;
        resultRange.end = close->range.end;
    }

    if ((inferredSignature && typedParameters.value_or(false)) ||
        (!inferredSignature && typedParameters.has_value() &&
         !*typedParameters))
        _diagnostics.Report(
            diagnostics::DiagnosticCode::UnexpectedToken, first.range,
            inferredSignature
                ? "contextually inferred anonymous functions use bare "
                  "parameter names"
                : "explicit anonymous functions require a type for every "
                  "parameter");

    std::string borrowSource;
    source::SourceRange borrowSourceRange;
    if (Match(TokenType::LeftArrow)) {
        const Token* source =
            Consume(TokenType::Identifier, "borrow source after '<-'");
        if (source == nullptr) return nullptr;
        borrowSource = source->value;
        borrowSourceRange = source->range;
        if (inferredSignature)
            _diagnostics.Report(
                diagnostics::DiagnosticCode::UnexpectedToken, source->range,
                "a contextually inferred anonymous function inherits its "
                "borrow contract");
    }

    auto body = ParseBlockStatement();
    if (body == nullptr) return nullptr;
    const source::SourceRange range{start, body->Range().end};
    std::unique_ptr<TypeNode> returnType;
    if (inferredSignature || results.empty()) {
        returnType =
            std::make_unique<TypeNode>(resultRange, PrimitiveType::Void);
    } else if (results.size() == 1) {
        returnType = std::move(results.front());
    } else {
        returnType =
            std::make_unique<TypeNode>(resultRange, std::move(results));
    }

    std::vector<FunctionModifier> modifiers;
    if (isUnsafe) modifiers.push_back(FunctionModifier::Unsafe);
    const std::string name = "$anon" + std::to_string(_rangeId++);
    auto function = std::make_unique<FunctionDeclaration>(
        range, std::move(modifiers), std::move(returnType), name,
        std::move(parameters), std::move(body), first.range, "",
        std::move(borrowSource), borrowSourceRange);
    return std::make_unique<AnonymousFunctionExpression>(
        range, std::move(function), std::move(captures), inferredSignature,
        explicitCaptureList);
}

std::optional<std::uint64_t> Parser::ParseArrayLength() {
    const Token* count = Consume(TokenType::NumberLiteral,
                                 "nonnegative integer literal array length");
    if (count == nullptr) return std::nullopt;
    std::uint64_t length = 0;
    const auto result = std::from_chars(
        count->value.data(), count->value.data() + count->value.size(), length);
    if (result.ec != std::errc{} ||
        result.ptr != count->value.data() + count->value.size()) {
        _diagnostics.Report(diagnostics::DiagnosticCode::InvalidArrayLength,
                            count->range, "array length is too large");
        return std::nullopt;
    }
    return length;
}

std::unique_ptr<Expression> Parser::ParseArrayExpression() {
    const source::SourceLocation start = Advance().range.start;
    std::vector<std::unique_ptr<Expression>> elements;
    bool repeat = false;
    std::uint64_t length = 0;
    if (!Check(TokenType::RBracket)) {
        for (;;) {
            std::unique_ptr<Expression> element = ParseExpression();
            if (element == nullptr) return nullptr;
            elements.push_back(std::move(element));
            if (elements.size() == 1 && Match(TokenType::SemiColon)) {
                const std::optional<std::uint64_t> count = ParseArrayLength();
                if (!count.has_value()) return nullptr;
                repeat = true;
                length = *count;
                break;
            }
            if (!Match(TokenType::Comma) || Check(TokenType::RBracket)) break;
        }
    }
    const Token* close =
        Consume(TokenType::RBracket, "']' after array initializer");
    if (close == nullptr) return nullptr;
    return std::make_unique<ArrayExpression>(
        source::SourceRange{start, close->range.end}, std::move(elements),
        repeat, length);
}

std::unique_ptr<Expression> Parser::ParseMatchExpression() {
    const Token& keyword = Advance();
    std::unique_ptr<Expression> value = ParseExpression();
    if (value == nullptr) return nullptr;
    if (!Consume(TokenType::LCurly, "'{' after match value")) return nullptr;

    std::vector<std::unique_ptr<MatchArm>> arms;
    while (!Check(TokenType::RCurly) && !IsAtEnd()) {
        const Token* first =
            Consume(TokenType::Identifier, "enum pattern or '_' in match arm");
        if (first == nullptr) return nullptr;

        const bool wildcard = first->value == "_";
        std::string enumName;
        std::string variant;
        source::SourceRange patternRange = first->range;
        if (!wildcard) {
            enumName = first->value;
            if (_importAliases.contains(enumName) && Match(TokenType::Scope)) {
                const Token* name = Consume(TokenType::Identifier,
                                            "enum name after module qualifier");
                if (name == nullptr) return nullptr;
                enumName += "::" + name->value;
                patternRange.end = name->range.end;
            }
            if (!Consume(TokenType::Dot, "'.' between enum and variant"))
                return nullptr;
            const Token* member =
                Consume(TokenType::Identifier, "enum variant in match arm");
            if (member == nullptr) return nullptr;
            variant = member->value;
            patternRange.end = member->range.end;
        }

        if (!Consume(TokenType::FatArrow, "'=>' after match pattern"))
            return nullptr;
        std::unique_ptr<Expression> result = ParseExpression();
        if (result == nullptr) return nullptr;
        const source::SourceRange armRange{patternRange.start,
                                           result->Range().end};
        arms.push_back(std::make_unique<MatchArm>(
            armRange, wildcard, std::move(enumName), std::move(variant),
            patternRange, std::move(result)));

        if (!Match(TokenType::Comma)) {
            if (!Check(TokenType::RCurly))
                Consume(TokenType::Comma, "',' between match arms");
            break;
        }
    }

    const Token* close = Consume(TokenType::RCurly, "'}' after match arms");
    const source::SourceLocation end =
        close == nullptr ? value->Range().end : close->range.end;
    return std::make_unique<MatchExpression>(
        source::SourceRange{keyword.range.start, end}, std::move(value),
        std::move(arms));
}

std::unique_ptr<Expression> Parser::ParsePrimaryExpression() {
    const Token& token = Current();
    if (IsAnonymousFunctionStart()) return ParseAnonymousFunctionExpression();
    switch (token.type) {
        case TokenType::Build: {
            Advance();
            auto type = ParseType();
            if (!type) return nullptr;
            auto body = ParseBlockStatement();
            if (!body) return nullptr;
            const source::SourceRange range{token.range.start,
                                            body->Range().end};
            return std::make_unique<BuildExpression>(range, std::move(type),
                                                     std::move(body));
        }
        case TokenType::Match:
            return ParseMatchExpression();
        case TokenType::LBracket:
            return ParseArrayExpression();
        case TokenType::NumberLiteral:
        case TokenType::FloatLiteral:
        case TokenType::StringLiteral:
        case TokenType::CharLiteral:
        case TokenType::True:
        case TokenType::False:
        case TokenType::None:
        case TokenType::Null:
            Advance();
            return std::make_unique<LiteralExpression>(token.range, token.type,
                                                       token.value);
        case TokenType::Identifier: {
            Advance();
            std::string name = token.value;
            auto range = token.range;
            if (_importAliases.contains(name) && Match(TokenType::Scope)) {
                const Token* member =
                    Consume(TokenType::Identifier,
                            "declaration name after module qualifier");
                if (!member) return nullptr;
                name += "::" + member->value;
                range.end = member->range.end;
            }
            return std::make_unique<NameExpression>(range, name);
        }
        case TokenType::LParen: {
            const Token& openParen = Advance();
            std::unique_ptr<Expression> expression = ParseExpression();
            if (expression == nullptr) {
                return nullptr;
            }
            if (Match(TokenType::Comma)) {
                std::vector<std::unique_ptr<Expression>> elements;
                elements.push_back(std::move(expression));
                while (!Check(TokenType::RParen)) {
                    auto element = ParseExpression();
                    if (element == nullptr) return nullptr;
                    elements.push_back(std::move(element));
                    if (!Match(TokenType::Comma)) break;
                }
                const Token* close =
                    Consume(TokenType::RParen, "')' after tuple expression");
                if (close == nullptr) return nullptr;
                if (elements.size() < 2) {
                    ReportUnexpectedToken(
                        "after tuple expression: at least two elements are "
                        "required");
                    return nullptr;
                }
                return std::make_unique<TupleExpression>(
                    source::SourceRange{openParen.range.start,
                                        close->range.end},
                    std::move(elements));
            }
            const Token* closeParen =
                Consume(TokenType::RParen, "')' after expression");
            const source::SourceLocation end = closeParen == nullptr
                                                   ? expression->Range().end
                                                   : closeParen->range.end;
            return std::make_unique<ParenthesizedExpression>(
                source::SourceRange{openParen.range.start, end},
                std::move(expression));
        }
        default:
            _diagnostics.Report(diagnostics::DiagnosticCode::ExpectedToken,
                                token.range, "expected an expression");
            return nullptr;
    }
}

void Parser::SynchronizeTopLevel() {
    // Parse() guarantees progress if a failed declaration consumed nothing.
    // Keep a following declaration's first token after a partially parsed one.
    while (!IsAtEnd() && !StartsType() && !Check(TokenType::Func) &&
           !Check(TokenType::Unsafe) && !Check(TokenType::Structure) &&
           !Check(TokenType::Private) && !Check(TokenType::Test)) {
        Advance();
    }
}

void Parser::SynchronizeStatement() {
    if (!IsAtEnd()) Advance();
    while (!IsAtEnd() && !Check(TokenType::SemiColon) &&
           !Check(TokenType::RCurly)) {
        Advance();
    }
    Match(TokenType::SemiColon);
}

void Parser::ParseHeader() {
    if (_headerParsed) return;
    _headerParsed = true;
    auto& moduleName = _moduleName;
    auto& imports = _imports;
    if (Match(TokenType::Module)) {
        const Token* name = Consume(TokenType::Identifier, "module name");
        if (name) moduleName = name->value;
        Consume(TokenType::SemiColon, "';' after module declaration");
    }
    if (Match(TokenType::At)) {
        const Token* directive =
            Consume(TokenType::Identifier, "imports after '@'");
        if (!directive || directive->value != "imports")
            ReportUnexpectedToken("in @imports directive");
        while (Match(TokenType::Scope)) {
            Import import;
            import.range.start = Current().range.start;
            const Token* segment =
                Consume(TokenType::Identifier, "import path");
            if (!segment) break;
            import.path.push_back(segment->value);
            // A new entry's ':: root' or ':: lib' is distinguished from a
            // path separator by its reserved import-root segment.
            while (Check(TokenType::Scope) &&
                   Peek(1).type == TokenType::Identifier &&
                   Peek(1).value != "root" && Peek(1).value != "lib") {
                Advance();
                import.path.push_back(Advance().value);
            }
            const bool selection =
                Check(TokenType::Scope) && (Peek(1).type == TokenType::Mul ||
                                            Peek(1).type == TokenType::LCurly);
            if (selection) Advance();
            if (selection && Match(TokenType::LCurly)) {
                do {
                    const Token* name =
                        Consume(TokenType::Identifier, "selected module name");
                    if (!name) break;
                    Import selected = import;
                    selected.path.push_back(name->value);
                    selected.alias = name->value;
                    selected.range.end = Current().range.start;
                    if (name->value == "root" || name->value == "lib")
                        ReportUnexpectedToken("reserved import path word");
                    imports.push_back(std::move(selected));
                } while (Match(TokenType::Comma));
                Consume(TokenType::RCurly, "'}' after selected modules");
            } else {
                import.wildcard = selection && Match(TokenType::Mul);
                if (!import.wildcard) {
                    import.alias = import.path.back();
                    if (Match(TokenType::As)) {
                        const Token* alias =
                            Consume(TokenType::Identifier, "import alias");
                        if (!alias) break;
                        import.alias = alias->value;
                    }
                }
                import.range.end = Current().range.start;
                imports.push_back(std::move(import));
            }
        }
        if (imports.empty())
            ReportUnexpectedToken("in empty @imports directive");
        Consume(TokenType::SemiColon, "';' after imports block");
        if (moduleName.empty())
            _diagnostics.Report(diagnostics::DiagnosticCode::UnexpectedToken,
                                directive ? directive->range : Current().range,
                                "@imports requires a module declaration");
    }

    for (const auto& import : imports) {
        const auto minimum = import.path[0] == "lib" ? 3u : 2u;
        if ((import.path[0] != "root" && import.path[0] != "lib") ||
            import.path.size() + (import.wildcard ? 1u : 0u) < minimum)
            _diagnostics.Report(
                diagnostics::DiagnosticCode::UnexpectedToken, import.range,
                "imports require root::<module> or lib::<library>::<module>");
    }
}

void Parser::SetImports(std::vector<Import> imports) {
    _imports = std::move(imports);
}

std::unique_ptr<CompilationUnit> Parser::Parse() {
    const source::SourceLocation start = _tokens.front().range.start;
    ParseHeader();
    for (const auto& import : _imports) {
        if (!import.wildcard && !_importAliases.insert(import.alias).second)
            _diagnostics.Report(
                diagnostics::DiagnosticCode::DuplicateDeclaration, import.range,
                "duplicate import alias '" + import.alias + "'");
    }
    std::vector<std::unique_ptr<Declaration>> declarations;
    while (!IsAtEnd()) {
        const std::size_t startPosition = _position;
        std::unique_ptr<Declaration> declaration = ParseDeclaration();
        if (declaration != nullptr) {
            declarations.push_back(std::move(declaration));
        } else {
            SynchronizeTopLevel();
        }
        if (_position == startPosition && !IsAtEnd()) {
            Advance();
        }
    }

    auto unit = std::make_unique<CompilationUnit>(
        source::SourceRange{start, Current().range.end},
        std::move(declarations));
    unit->moduleName = std::move(_moduleName);
    unit->imports = std::move(_imports);
    return unit;
}

}  // namespace flecha::core
