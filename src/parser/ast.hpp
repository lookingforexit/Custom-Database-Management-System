#pragma once

// this file defines the sql ast used by parsing and planning stages.
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "common/types.hpp"

namespace dbms::parser {

enum class AggregateKind {
    kSum,
    kCount,
    kAvg,
};

enum class ComparisonOperator {
    kEqual,
    kNotEqual,
    kLess,
    kGreater,
    kLessEqual,
    kGreaterEqual,
};

enum class LogicalOperator {
    kAnd,
    kOr,
};

using LiteralValue = common::Value;

struct Expression;

struct LiteralExpression {
    LiteralValue value;
};

struct ColumnReferenceExpression {
    std::optional<std::string> database_name;
    std::optional<std::string> table_name;
    std::string column_name;
};

struct BinaryComparisonExpression {
    ComparisonOperator op;
    std::unique_ptr<Expression> left;
    std::unique_ptr<Expression> right;
};

struct LogicalExpression {
    LogicalOperator op;
    std::unique_ptr<Expression> left;
    std::unique_ptr<Expression> right;
};

struct BetweenExpression {
    std::unique_ptr<Expression> value;
    std::unique_ptr<Expression> lower_bound;
    std::unique_ptr<Expression> upper_bound;
};

struct LikeExpression {
    std::unique_ptr<Expression> value;
    std::unique_ptr<Expression> pattern;
};

struct AggregateCallExpression {
    AggregateKind kind;
    std::string column_name;
};

struct Expression {
    using Variant = std::variant<
        LiteralExpression,
        ColumnReferenceExpression,
        BinaryComparisonExpression,
        LogicalExpression,
        BetweenExpression,
        LikeExpression,
        AggregateCallExpression>;

    Variant node;
};

struct QualifiedName {
    std::optional<std::string> database_name;
    std::string object_name;
};

struct ColumnDefinition {
    std::string name;
    common::ValueType type{common::ValueType::kNull};
    bool not_null{false};
    bool indexed{false};
    std::optional<LiteralValue> default_value;
};

struct SelectItem {
    bool is_wildcard{false};
    std::string column_name;
    std::optional<std::string> alias;
    std::optional<AggregateKind> aggregate;
};

struct Assignment {
    std::string column_name;
    Expression value;
};

struct CreateDatabaseStatement {
    std::string database_name;
};

struct DropDatabaseStatement {
    std::string database_name;
};

struct UseDatabaseStatement {
    std::string database_name;
};

struct CreateTableStatement {
    QualifiedName table_name;
    std::vector<ColumnDefinition> columns;
};

struct DropTableStatement {
    QualifiedName table_name;
};

struct InsertStatement {
    QualifiedName table_name;
    std::vector<std::string> column_names;
    std::vector<std::vector<Expression>> rows;
};

struct UpdateStatement {
    QualifiedName table_name;
    std::vector<Assignment> assignments;
    std::optional<Expression> where;
};

struct DeleteStatement {
    QualifiedName table_name;
    std::optional<Expression> where;
};

struct SelectStatement {
    QualifiedName table_name;
    std::vector<SelectItem> items;
    std::optional<Expression> where;
};

struct RevertStatement {
    QualifiedName table_name;
    std::string timestamp;
};

struct UnknownStatement {
    std::string raw_sql;
};

using Statement = std::variant<
    UnknownStatement,
    CreateDatabaseStatement,
    DropDatabaseStatement,
    UseDatabaseStatement,
    CreateTableStatement,
    DropTableStatement,
    InsertStatement,
    UpdateStatement,
    DeleteStatement,
    SelectStatement,
    RevertStatement>;

}  // namespace dbms::parser
