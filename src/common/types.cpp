#include "common/types.hpp"

namespace dbms::common {

    ValueType GetValueType(const Value& value) {
        if (std::holds_alternative<std::monostate>(value)) {
            return ValueType::kNull;
        }
        if (std::holds_alternative<std::int64_t>(value)) {
            return ValueType::kInt64;
        }
        return ValueType::kString;
    }

    std::string ValueToString(const Value& value) {
        if (std::holds_alternative<std::monostate>(value)) {
            return "NULL";
        }
        if (std::holds_alternative<std::int64_t>(value)) {
            return std::to_string(std::get<std::int64_t>(value));
        }
        return std::get<std::string>(value);
    }

}  // namespace dbms::common