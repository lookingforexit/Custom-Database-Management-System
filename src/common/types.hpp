#pragma once

// this file holds shared primitive types used across the whole project.
#include <cstdint>
#include <optional>
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

struct NamedValue {
    std::string name;
    Value value;
};

using Row = std::vector<NamedValue>;

}  // namespace dbms::common
