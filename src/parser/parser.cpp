#include "parser/parser.hpp"

// this file will parse ddl, dml, revert, filters, and aggregates.
namespace dbms::parser {

common::Result<Statement> Parser::Parse(const std::string& sql) const {
    Statement statement = UnknownStatement{.raw_sql = sql};
    return {.value = std::move(statement), .error = std::nullopt};
}

}  // namespace dbms::parser
