#pragma once

// this file defines shared primitive types used across the whole project.
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace dbms::common {

    enum class ValueType {
        kNull,
        kInt64,
        kString,
    };

    using Value = std::variant<std::monostate, std::int64_t, std::string>;
    using RowId = std::uint64_t;
    using ShardId = std::uint32_t;

    struct RowData {
        std::vector<Value> values;
    };

    // helpers

    inline bool IsNull(const Value &value) {
        return std::holds_alternative<std::monostate>(value);
    }

    // default comparison logic for non-null, but null == null
    int64_t CompareValues(const Value& lhs, const Value& rhs);
    ValueType GetValueType(const Value &value);
    std::string ValueToString(const Value &value);
    bool CanAssignToType(const Value& value, ValueType type, bool allow_null);

} // namespace dbms::common
