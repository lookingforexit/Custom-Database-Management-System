#include "common/types.hpp"

#include <stdexcept>

// this file implements shared value typing and string conversion helpers.
namespace dbms::common {

    int64_t CompareValues(const Value& lhs, const Value& rhs) {
        if (GetValueType(lhs) != GetValueType(rhs)) {
            throw std::logic_error("mismatched types");
        }

        auto type = GetValueType(lhs);
        if (type == ValueType::kNull) {
            return 0;
        }
        if (type == ValueType::kInt64) {
            return std::get<int64_t>(lhs) - std::get<int64_t>(rhs);
        }

        return AsString(lhs).compare(AsString(rhs));
    }

    ValueType GetValueType(const Value &value) {
        if (std::holds_alternative<std::monostate>(value)) {
            return ValueType::kNull;
        }
        if (std::holds_alternative<std::int64_t>(value)) {
            return ValueType::kInt64;
        }
        return ValueType::kString;
    }

    std::string ValueToString(const Value &value) {
        if (std::holds_alternative<std::monostate>(value)) {
            return "NULL";
        }
        if (std::holds_alternative<std::int64_t>(value)) {
            return std::to_string(std::get<std::int64_t>(value));
        }
        return std::string(AsString(value));
    }

    bool CanAssignToType(const Value &value, ValueType type, bool allow_null) {
        if (IsNull(value)) {
            return allow_null;
        }

        if (GetValueType(value) == type) {
            return true;
        }

        return false;
    }

} // namespace dbms::common
