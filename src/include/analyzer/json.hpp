#ifndef FLECHA_ANALYZER_JSON_HPP
#define FLECHA_ANALYZER_JSON_HPP

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace flecha::analyzer::json {

struct Value final {
    using Object = std::map<std::string, Value>;
    using Array = std::vector<Value>;
    using Storage =
        std::variant<std::nullptr_t, bool, std::string, Object, Array>;

    Storage value;
    bool number = false;
};

[[nodiscard]] std::optional<Value> Parse(std::string_view text,
                                         std::string& error);
[[nodiscard]] const Value::Object* AsObject(const Value& value);
[[nodiscard]] const Value::Array* AsArray(const Value& value);
[[nodiscard]] const std::string* AsString(const Value& value);
[[nodiscard]] const std::string* AsNumber(const Value& value);
[[nodiscard]] const Value* Field(const Value::Object& object,
                                 std::string_view name);
[[nodiscard]] std::string Quote(std::string_view value);

}  // namespace flecha::analyzer::json

#endif  // FLECHA_ANALYZER_JSON_HPP
