#pragma once

// this file defines shared primitive types used across the whole project.
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace dbms::common {

    enum class ValueType {
        kNull,
        kInt64,
        kString,
    };

    using RowId = std::uint64_t;
    using ShardId = std::uint32_t;
    using InternedString = std::shared_ptr<const std::string>;
    inline constexpr RowId kInvalidRowId = std::numeric_limits<RowId>::max();

    using Value =
        std::variant<std::monostate, std::int64_t, std::string, InternedString>;

    struct RowData {
        RowId row_id{kInvalidRowId};
        std::vector<Value> values;
    };

    // helpers

    inline bool IsNull(const Value &value) {
        return std::holds_alternative<std::monostate>(value);
    }

    inline bool IsStringValue(const Value &value) {
        return std::holds_alternative<std::string>(value) ||
               std::holds_alternative<InternedString>(value);
    }

    inline const std::string &AsString(const Value &value) {
        if (std::holds_alternative<std::string>(value)) {
            return std::get<std::string>(value);
        }
        return *std::get<InternedString>(value);
    }

    // default comparison logic for non-null, but null == null
    int64_t CompareValues(const Value& lhs, const Value& rhs);
    ValueType GetValueType(const Value &value);
    std::string ValueToString(const Value &value);
    bool CanAssignToType(const Value& value, ValueType type, bool allow_null);

} // namespace dbms::common
