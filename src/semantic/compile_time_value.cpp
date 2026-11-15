#include "semantic/compile_time_value.hpp"

#include <bit>
#include <cmath>
#include <limits>
#include <unordered_set>

#include "core/integer_literal.hpp"

namespace flecha::semantic {
namespace {
using namespace core;
using K = SemanticTypeKind;
using Value = std::optional<CompileTimeValue>;
unsigned Width(SemanticType type) {
    switch (type.Kind()) {
        case K::I8:
        case K::U8:
        case K::Char:
            return 8;
        case K::I16:
        case K::U16:
            return 16;
        case K::I32:
        case K::U32:
            return 32;
        default:
            return 64;
    }
}
bool Signed(SemanticType type) {
    return type.Kind() >= K::I8 && type.Kind() <= K::ISize;
}
std::int64_t SignedValue(std::uint64_t value, SemanticType type) {
    unsigned width = Width(type);
    if (width < 64 && (value & (std::uint64_t{1} << (width - 1))))
        value |= ~((std::uint64_t{1} << width) - 1);
    return std::bit_cast<std::int64_t>(value);
}
Value Convert(CompileTimeValue value, SemanticType type) {
    if (type.IsError() || type.Kind() == K::None) return value;
    if (value.type.Kind() == K::None) {
        if (type.IsInteger()) return CompileTimeValue{type, std::uint64_t{0}};
        if (type.IsFloating()) return CompileTimeValue{type, 0.0};
        if (type.Kind() == K::Bool) return CompileTimeValue{type, false};
        if (type.Kind() == K::Char) return CompileTimeValue{type, '\0'};
        if (type.Kind() == K::StringView)
            return CompileTimeValue{type, std::string{}};
        if (type.Kind() == K::Enum && type.Enum()->defaultValue)
            return CompileTimeValue{
                type,
                EnumMetadataValue{type.Enum(), *type.Enum()->defaultValue}};
    }
    auto number = [&]() -> long double {
        if (auto p = std::get_if<double>(&value.value)) return *p;
        if (auto p = std::get_if<std::uint64_t>(&value.value))
            return Signed(value.type)
                       ? static_cast<long double>(SignedValue(*p, value.type))
                       : *p;
        if (auto p = std::get_if<bool>(&value.value)) return *p;
        if (auto p = std::get_if<char>(&value.value))
            return static_cast<unsigned char>(*p);
        if (auto p = std::get_if<EnumMetadataValue>(&value.value))
            return p->value;
        return 0;
    };
    if (type.IsInteger() || type.Kind() == K::Char) {
        std::uint64_t bits;
        if (auto p = std::get_if<std::uint64_t>(&value.value)) {
            bits = Signed(value.type)
                       ? static_cast<std::uint64_t>(SignedValue(*p, value.type))
                       : *p;
        } else {
            const auto n = number();
            if (!std::isfinite(n) ||
                n < static_cast<long double>(
                        std::numeric_limits<std::int64_t>::min()) ||
                n >= std::ldexp(1.0L, 64))
                return {};
            bits =
                n < 0 ? static_cast<std::uint64_t>(static_cast<std::int64_t>(n))
                      : static_cast<std::uint64_t>(n);
        }
        if (Width(type) < 64) bits &= (std::uint64_t{1} << Width(type)) - 1;
        if (type.Kind() == K::Char)
            value.value = static_cast<char>(bits);
        else
            value.value = bits;
    } else if (type.IsFloating()) {
        double n = static_cast<double>(number());
        value.value = type.Kind() == K::F32
                          ? static_cast<double>(static_cast<float>(n))
                          : n;
    } else if (type.Kind() == K::Bool)
        value.value = number() != 0;
    value.type = type.WithoutConst();
    return value;
}
class Evaluator {
   public:
    const SemanticModel& model;
    const MetadataResolver& resolver;
    std::unordered_set<const Symbol*> active;
    Value Eval(const Expression& e) {
        if (resolver)
            if (auto v = resolver(e)) return v;
        auto type = model.TypeOf(e).WithoutConst();
        switch (e.Kind()) {
            case AstNodeKind::LiteralExpression: {
                const auto& n = static_cast<const LiteralExpression&>(e);
                switch (n.LiteralType()) {
                    case TokenType::True:
                    case TokenType::False:
                        return CompileTimeValue{
                            SemanticType(K::Bool),
                            n.LiteralType() == TokenType::True};
                    case TokenType::StringLiteral:
                        return CompileTimeValue{SemanticType(K::StringView),
                                                n.Value()};
                    case TokenType::CharLiteral:
                        return CompileTimeValue{
                            SemanticType(K::Char),
                            n.Value().empty() ? '\0' : n.Value()[0]};
                    case TokenType::NumberLiteral: {
                        std::uint64_t v;
                        if (!ParseUnsignedIntegerLiteral(n.Value(), v))
                            return {};
                        return Convert(
                            {SemanticType(K::U64), v},
                            type.IsError() ? SemanticType(K::I32) : type);
                    }
                    case TokenType::FloatLiteral: {
                        try {
                            return Convert(
                                {SemanticType(K::F64), std::stod(n.Value())},
                                type);
                        } catch (...) {
                            return {};
                        }
                    }
                    case TokenType::None:
                        return CompileTimeValue{
                            type.IsError() ? SemanticType(K::None) : type,
                            ErrorMetadataValue{"", 0}};
                    default:
                        return {};
                }
            }
            case AstNodeKind::NameExpression: {
                const auto* s =
                    model.SymbolFor(static_cast<const NameExpression&>(e));
                if (!s || !active.insert(s).second) return {};
                const auto* c = model.ModuleConstantFor(*s);
                auto value = c ? Eval(c->Initializer()) : Value{};
                active.erase(s);
                return value ? Convert(*value, s->Type()) : Value{};
            }
            case AstNodeKind::ParenthesizedExpression:
                return Eval(
                    static_cast<const ParenthesizedExpression&>(e).Inner());
            case AstNodeKind::CastExpression: {
                const auto& cast = static_cast<const CastExpression&>(e);
                auto v = Eval(cast.Value());
                if (type.IsError())
                    type = SemanticType::FromSyntax(cast.TargetType());
                if (!v || type.IsError() || !CanExplicitlyCast(v->type, type))
                    return {};
                return Convert(*v, type);
            }
            case AstNodeKind::IsOfExpression:
                if (!type.IsError())
                    return CompileTimeValue{
                        SemanticType(K::Bool),
                        static_cast<const IsOfExpression&>(e).Result()};
                return {};
            case AstNodeKind::MemberAccessExpression: {
                const auto& n = static_cast<const MemberAccessExpression&>(e);
                if (model.FieldFor(n)) return {};
                if (type.Kind() == K::Enum) {
                    for (auto& v : type.Enum()->variants)
                        if (v.name == n.Member())
                            return CompileTimeValue{
                                type, EnumMetadataValue{type.Enum(), v.value}};
                } else if (type.Kind() == K::NamedError) {
                    auto& variants = type.Family().variants;
                    for (std::size_t i = 0; i < variants.size(); ++i)
                        if (variants[i] == n.Member())
                            return CompileTimeValue{
                                type,
                                ErrorMetadataValue{type.Family().name, i + 1}};
                } else if (type.IsInteger()) {
                    const auto max =
                        Signed(type)
                            ? (std::uint64_t{1} << (Width(type) - 1)) - 1
                            : (Width(type) == 64
                                   ? ~std::uint64_t{0}
                                   : (std::uint64_t{1} << Width(type)) - 1);
                    return CompileTimeValue{type, n.Member() == "max_value"
                                                      ? max
                                                  : Signed(type) ? max + 1
                                                                 : 0};
                } else if (type.IsFloating()) {
                    const bool f = type.Kind() == K::F32;
                    double v = std::numeric_limits<double>::quiet_NaN();
                    if (n.Member() == "infinity") v = INFINITY;
                    if (n.Member() == "negative_infinity") v = -INFINITY;
                    if (n.Member() == "max_value")
                        v = f ? std::numeric_limits<float>::max()
                              : std::numeric_limits<double>::max();
                    if (n.Member() == "min_value")
                        v = f ? std::numeric_limits<float>::lowest()
                              : std::numeric_limits<double>::lowest();
                    if (n.Member() == "min_positive")
                        v = f ? std::numeric_limits<float>::min()
                              : std::numeric_limits<double>::min();
                    if (n.Member() == "epsilon")
                        v = f ? std::numeric_limits<float>::epsilon()
                              : std::numeric_limits<double>::epsilon();
                    return CompileTimeValue{type, v};
                }
                return {};
            }
            case AstNodeKind::UnaryExpression: {
                auto& n = static_cast<const UnaryExpression&>(e);
                auto v = Eval(n.Operand());
                if (!v) return {};
                if (n.OperatorType() == TokenType::Bang) {
                    auto p = std::get_if<bool>(&v->value);
                    return p ? Value{CompileTimeValue{SemanticType(K::Bool),
                                                      !*p}}
                             : Value{};
                }
                if (auto p = std::get_if<std::uint64_t>(&v->value)) {
                    if (n.OperatorType() == TokenType::Sub)
                        *p = 0 - *p;
                    else if (n.OperatorType() == TokenType::BitNot)
                        *p = ~*p;
                    else if (n.OperatorType() != TokenType::Add)
                        return {};
                    return Convert(*v, type.IsError() ? v->type : type);
                }
                if (auto p = std::get_if<double>(&v->value)) {
                    if (n.OperatorType() == TokenType::Sub)
                        *p = -*p;
                    else if (n.OperatorType() != TokenType::Add)
                        return {};
                    return v;
                }
                return {};
            }
            case AstNodeKind::BinaryExpression: {
                auto& n = static_cast<const BinaryExpression&>(e);
                auto l = Eval(n.Left());
                if (!l) return {};
                const auto op = n.OperatorType();
                auto boolean = [](bool b) -> Value {
                    return CompileTimeValue{SemanticType(K::Bool), b};
                };
                if (auto b = std::get_if<bool>(&l->value)) {
                    if (op == TokenType::And && !*b) return boolean(false);
                    if (op == TokenType::Or && *b) return boolean(true);
                }
                auto r = Eval(n.Right());
                if (!r) return {};
                // Match the ordinary checker's contextual integer literals.
                // Never fold a type error into a false branch and hide its
                // diagnostic.
                if (l->type.IsInteger() && r->type.IsInteger() &&
                    op != TokenType::ShiftLeft && op != TokenType::ShiftRight &&
                    n.Right().Kind() == AstNodeKind::LiteralExpression) {
                    const auto bits = std::get<std::uint64_t>(r->value);
                    const unsigned width = Width(l->type);
                    const auto limit =
                        Signed(l->type) ? (std::uint64_t{1} << (width - 1)) - 1
                        : width == 64   ? ~std::uint64_t{0}
                                        : (std::uint64_t{1} << width) - 1;
                    if (bits > limit) return {};
                    r = Convert(*r, l->type);
                    if (!r) return {};
                }
                if (l->type.WithoutConst() != r->type.WithoutConst() &&
                    !(l->type.IsErrorValue() && r->type.IsErrorValue()) &&
                    op != TokenType::ShiftLeft && op != TokenType::ShiftRight)
                    return {};
                if (op == TokenType::Equal || op == TokenType::StrictEqual)
                    return boolean(l->value == r->value);
                if (op == TokenType::NotEqual ||
                    op == TokenType::StrictNotEqual)
                    return boolean(l->value != r->value);
                if (auto a = std::get_if<bool>(&l->value)) {
                    auto b = std::get_if<bool>(&r->value);
                    if (!b) return {};
                    if (op == TokenType::And) return boolean(*a && *b);
                    if (op == TokenType::Or) return boolean(*a || *b);
                }
                if (auto a = std::get_if<char>(&l->value)) {
                    auto b = std::get_if<char>(&r->value);
                    if (!b) return {};
                    const auto x = static_cast<unsigned char>(*a),
                               y = static_cast<unsigned char>(*b);
                    switch (op) {
                        case TokenType::Less:
                            return boolean(x < y);
                        case TokenType::LessEqual:
                            return boolean(x <= y);
                        case TokenType::Greater:
                            return boolean(x > y);
                        case TokenType::GreaterEqual:
                            return boolean(x >= y);
                        default:
                            return {};
                    }
                }
                if (auto a = std::get_if<std::uint64_t>(&l->value)) {
                    auto b = std::get_if<std::uint64_t>(&r->value);
                    if (!b) return {};
                    bool sign = Signed(l->type);
                    auto sa = SignedValue(*a, l->type),
                         sb = SignedValue(*b, r->type);
                    std::uint64_t v;
                    switch (op) {
                        case TokenType::Add:
                            v = *a + *b;
                            break;
                        case TokenType::Sub:
                            v = *a - *b;
                            break;
                        case TokenType::Mul:
                            v = *a * *b;
                            break;
                        case TokenType::Div:
                        case TokenType::Mod:
                            if (!*b ||
                                (sign &&
                                 sa ==
                                     std::numeric_limits<std::int64_t>::min() &&
                                 sb == -1))
                                return {};
                            v = op == TokenType::Div
                                    ? (sign
                                           ? static_cast<std::uint64_t>(sa / sb)
                                           : *a / *b)
                                    : (sign
                                           ? static_cast<std::uint64_t>(sa % sb)
                                           : *a % *b);
                            break;
                        case TokenType::Ampersand:
                            v = *a & *b;
                            break;
                        case TokenType::BitOr:
                            v = *a | *b;
                            break;
                        case TokenType::Xor:
                            v = *a ^ *b;
                            break;
                        case TokenType::ShiftLeft:
                        case TokenType::ShiftRight:
                            if (*b >= Width(l->type)) return {};
                            v = op == TokenType::ShiftLeft ? *a << *b
                                : sign ? static_cast<std::uint64_t>(sa >> *b)
                                       : *a >> *b;
                            break;
                        case TokenType::Less:
                            return boolean(sign ? sa < sb : *a < *b);
                        case TokenType::LessEqual:
                            return boolean(sign ? sa <= sb : *a <= *b);
                        case TokenType::Greater:
                            return boolean(sign ? sa > sb : *a > *b);
                        case TokenType::GreaterEqual:
                            return boolean(sign ? sa >= sb : *a >= *b);
                        default:
                            return {};
                    }
                    return Convert({l->type, v},
                                   type.IsError() ? l->type : type);
                }
                if (auto a = std::get_if<double>(&l->value)) {
                    auto b = std::get_if<double>(&r->value);
                    if (!b) return {};
                    double v;
                    switch (op) {
                        case TokenType::Add:
                            v = *a + *b;
                            break;
                        case TokenType::Sub:
                            v = *a - *b;
                            break;
                        case TokenType::Mul:
                            v = *a * *b;
                            break;
                        case TokenType::Div:
                            v = *a / *b;
                            break;
                        case TokenType::Less:
                            return boolean(*a < *b);
                        case TokenType::LessEqual:
                            return boolean(*a <= *b);
                        case TokenType::Greater:
                            return boolean(*a > *b);
                        case TokenType::GreaterEqual:
                            return boolean(*a >= *b);
                        default:
                            return {};
                    }
                    return Convert({l->type, v},
                                   type.IsError() ? l->type : type);
                }
                return {};
            }
            default:
                return {};
        }
    }
};
}  // namespace
std::optional<CompileTimeValue> ConvertCompileTimeValue(CompileTimeValue value,
                                                        SemanticType type) {
    return Convert(std::move(value), type);
}
std::optional<CompileTimeValue> EvaluateCompileTimeValue(
    const core::Expression& expression, const SemanticModel& model,
    const MetadataResolver& resolver) {
    return Evaluator{model, resolver, {}}.Eval(expression);
}
}  // namespace flecha::semantic
