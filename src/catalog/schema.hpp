#pragma once

// this file describes database, table, column, and index metadata.
#include <optional>
#include <string>
#include <vector>

#include "common/types.hpp"

namespace dbms::catalog {

enum class ColumnConstraint {
    kNone,
    kNotNull,
    kIndexed,
};

struct ColumnDefinition {
    std::string name;
    common::ValueType type{common::ValueType::kNull};
    ColumnConstraint constraint{ColumnConstraint::kNone};
    std::optional<common::Value> default_value;
};

struct IndexDefinition {
    std::string name;
    std::string column_name;
    bool unique{false};
    bool is_primary_access_path{false};
};

struct TableSchema {
    std::string database_name;
    std::string table_name;
    std::vector<ColumnDefinition> columns;
    std::vector<IndexDefinition> indexes;
};

}  // namespace dbms::catalog
