#include <cassert>
#include <iostream>
#include <string>

#include "common/error.hpp"
#include "parser/ast.hpp"
#include "parser/parser.hpp"

namespace {

    void ExpectParseError(dbms::parser::Parser &parser, const std::string &sql) {
        const auto result = parser.Parse(sql);
        assert(!result.ok());
        assert(result.error.has_value());
        assert(result.error->code == dbms::common::ErrorCode::kParseError);
    }

    void ExpectParseOk(dbms::parser::Parser &parser, const std::string &sql) {
        const auto result = parser.Parse(sql);
        if (!result.ok()) {
            std::cerr << "Expected success, got error for SQL: " << sql << "\n";
            if (result.error.has_value()) {
                std::cerr << "Error: " << result.error->message << "\n";
            }
        }
        assert(result.ok());
    }

    std::optional<dbms::parser::Statement>
    MustParse(dbms::parser::Parser &parser, const std::string &sql) {
        auto result = parser.Parse(sql);
        if (!result.ok()) {
            std::cerr << "Parse failed for SQL: " << sql << "\n";
            if (result.error.has_value()) {
                std::cerr << "Error: " << result.error->message << "\n";
            }
            assert(false);
        }
        return std::move(*result.value);
    }

} // namespace

int main() {
    dbms::parser::Parser parser;

    // CREATE TABLE semantic checks.
    ExpectParseError(parser, "CREATE TABLE t ();");
    ExpectParseError(parser, "CREATE TABLE t (id INT, id STRING);");
    ExpectParseError(parser, "CREATE TABLE t (name STRING DEFAULT 123);");
    ExpectParseError(parser,
                     "CREATE TABLE t (id INT NOT_NULL DEFAULT NULL);");

    // INSERT semantic checks.
    ExpectParseError(parser, "INSERT INTO t () VALUE ();");
    ExpectParseError(parser, "INSERT INTO t (id, id) VALUE (1, 2);");
    ExpectParseError(parser, "INSERT INTO t (id, name) VALUE (1);");
    ExpectParseError(parser, "INSERT INTO t (id) VALUES;");
    ExpectParseError(parser, "INSERT INTO t (id) VALUE ;");
    ExpectParseError(parser, "INSERT INTO t (id) VALUE (1");
    ExpectParseOk(parser, "INSERT INTO t (id) VALUE (-1);");

    // UPDATE semantic checks.
    ExpectParseError(parser, "UPDATE t SET;");
    ExpectParseError(parser, "UPDATE t SET id = 1, id = 2;");
    ExpectParseError(parser, "UPDATE t SET id = ;");
    ExpectParseOk(parser, "UPDATE t SET id = -5 WHERE id == 1;");

    // SELECT semantic checks.
    ExpectParseError(parser, "SELECT *, id FROM t;");
    ExpectParseError(parser, "SELECT COUNT(id), id FROM t;");
    ExpectParseError(parser, "SELECT SUM(*) FROM t;");
    ExpectParseError(parser, "SELECT AVG(*) FROM t;");
    ExpectParseError(parser, "SELECT id FROM t WHERE (id == 1;");
    ExpectParseError(parser, "SELECT id FROM t WHERE id BETWEEN 1;");
    ExpectParseError(parser, "SeLeCt id FROM t;");

    // Positive controls.
    ExpectParseOk(parser, "CREATE TABLE t (id INT INDEXED, name STRING);");
    ExpectParseOk(parser, "INSERT INTO t (id, name) VALUE (1, \"A\");");
    ExpectParseOk(parser, "INSERT INTO t (id, name) VALUES (1, \"A\"), (2, \"B\");");
    ExpectParseOk(parser, "UPDATE t SET name = \"X\" WHERE id == 1;");
    ExpectParseOk(parser,
                  "SELECT id FROM t WHERE (id >= 1 AND id < 10) OR id == 15;");
    ExpectParseOk(parser, "REVERT t LATEST;");
    ExpectParseOk(parser, "REVERT t EXACT \"2026.05.20-12:00:00.000001\";");
    ExpectParseOk(parser,
                  "REVERT t AT_OR_BEFORE \"2026.05.20-12:00:00.000001\";");
    ExpectParseOk(parser, "BEGIN;");
    ExpectParseOk(parser, "BEGIN TRANSACTION;");
    ExpectParseOk(parser, "COMMIT;");
    ExpectParseOk(parser, "COMMIT TRANSACTION;");
    ExpectParseOk(parser, "ROLLBACK;");
    ExpectParseOk(parser, "ROLLBACK TRANSACTION;");
    ExpectParseOk(parser, "CHECK INDEX;");
    ExpectParseOk(parser, "REBUILD INDEX;");

    // AST shape checks: AND has higher precedence than OR.
    {
        auto stmt = MustParse(
            parser, "SELECT id FROM t WHERE id == 1 OR id == 2 AND id == 3;");
        assert(stmt.has_value());
        const auto *select =
            std::get_if<dbms::parser::SelectStatement>(&stmt.value());
        assert(select != nullptr);
        assert(select->where.has_value());

        const auto *top_logic = std::get_if<dbms::parser::LogicalExpression>(
            &select->where->node);
        assert(top_logic != nullptr);
        assert(top_logic->op == dbms::parser::LogicalOperator::kOr);

        const auto *right_logic = std::get_if<dbms::parser::LogicalExpression>(
            &top_logic->right->node);
        assert(right_logic != nullptr);
        assert(right_logic->op == dbms::parser::LogicalOperator::kAnd);
    }

    // AST shape checks: BETWEEN and LIKE are parsed as dedicated nodes.
    {
        auto stmt =
            MustParse(parser, "SELECT id FROM t WHERE name LIKE \"A.*\";");
        assert(stmt.has_value());
        const auto *select =
            std::get_if<dbms::parser::SelectStatement>(&stmt.value());
        assert(select != nullptr);
        assert(select->where.has_value());
        assert(std::holds_alternative<dbms::parser::LikeExpression>(
            select->where->node));
    }

    {
        auto stmt =
            MustParse(parser, "SELECT id FROM t WHERE id BETWEEN 1 AND 10;");
        assert(stmt.has_value());
        const auto *select =
            std::get_if<dbms::parser::SelectStatement>(&stmt.value());
        assert(select != nullptr);
        assert(select->where.has_value());
        assert(std::holds_alternative<dbms::parser::BetweenExpression>(
            select->where->node));
    }

    // AST checks for aggregates and aliases.
    {
        auto stmt =
            MustParse(parser, "SELECT SUM(score) AS total FROM t WHERE id >= 1;");
        assert(stmt.has_value());
        const auto *select =
            std::get_if<dbms::parser::SelectStatement>(&stmt.value());
        assert(select != nullptr);
        assert(select->items.size() == 1);
        assert(select->items[0].aggregate.has_value());
        assert(select->items[0].aggregate.value() ==
               dbms::parser::AggregateKind::kSum);
        assert(select->items[0].column_name == "score");
        assert(select->items[0].alias.has_value());
        assert(select->items[0].alias.value() == "total");
    }

    {
        auto stmt = MustParse(parser, "SELECT COUNT(*) AS c FROM t;");
        assert(stmt.has_value());
        const auto *select =
            std::get_if<dbms::parser::SelectStatement>(&stmt.value());
        assert(select != nullptr);
        assert(select->items.size() == 1);
        assert(select->items[0].aggregate.has_value());
        assert(select->items[0].aggregate.value() ==
               dbms::parser::AggregateKind::kCount);
        assert(select->items[0].column_name == "*");
    }

    return 0;
}
