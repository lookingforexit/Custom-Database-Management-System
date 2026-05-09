#pragma once

// this file defines the sql ast used by parsing and planning stages.
#include <memory>
#include <string>
#include <vector>

namespace dbms::parser {

enum class StatementKind {
    kUnknown,
    kCreateDatabase,
    kDropDatabase,
    kUseDatabase,
    kCreateTable,
    kDropTable,
    kInsert,
    kUpdate,
    kDelete,
    kSelect,
    kRevert,
};

enum class ExpressionKind {
    kLiteral,
    kColumnRef,
    kBinaryComparison,
    kLogicalAnd,
    kLogicalOr,
    kBetween,
    kLike,
    kAggregateCall,
};

struct Expression {
    ExpressionKind kind{ExpressionKind::kLiteral};
    std::string text;
    std::vector<std::unique_ptr<Expression>> children;
};

struct Statement {
    StatementKind kind{StatementKind::kUnknown};
    std::string target_name;
    std::vector<std::string> projection;
    std::unique_ptr<Expression> predicate;
};

}  // namespace dbms::parser
