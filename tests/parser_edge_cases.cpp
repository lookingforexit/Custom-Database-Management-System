#include <cassert>
#include <string>

#include "common/error.hpp"
#include "parser/parser.hpp"

namespace {

    void ExpectParseOk(dbms::parser::Parser &parser, const std::string &sql) {
        const auto result = parser.Parse(sql);
        assert(result.ok());
    }

    void ExpectParseError(dbms::parser::Parser &parser, const std::string &sql) {
        const auto result = parser.Parse(sql);
        assert(!result.ok());
        assert(result.error.has_value());
        assert(result.error->code == dbms::common::ErrorCode::kParseError);
    }

} // namespace

int main() {
    dbms::parser::Parser parser;

    // Negative numeric literals (recently added unary minus support).
    ExpectParseOk(parser, "INSERT INTO t (id) VALUE (-1);");
    ExpectParseOk(parser, "UPDATE t SET id = -42 WHERE id == 1;");
    ExpectParseOk(parser, "SELECT id FROM t WHERE id >= -7;");
    ExpectParseOk(parser, "CREATE TABLE t (id INT DEFAULT -5);");

    // Broken unary minus usage.
    ExpectParseError(parser, "INSERT INTO t (id) VALUE (-);");
    ExpectParseError(parser, "UPDATE t SET id = - WHERE id == 1;");
    ExpectParseError(parser, "SELECT id FROM t WHERE id == -;");

    // Parenthesis balance and logical chains.
    ExpectParseError(parser, "SELECT * FROM t WHERE (id == 1;");
    ExpectParseError(parser, "SELECT * FROM t WHERE id == 1));");
    ExpectParseOk(
        parser,
        "SELECT * FROM t WHERE (((id == 1) OR (id == 2)) AND (id != 3));");

    // BETWEEN/LIKE malformed variants.
    ExpectParseError(parser, "SELECT * FROM t WHERE id BETWEEN 1;");
    ExpectParseError(parser, "SELECT * FROM t WHERE id BETWEEN AND 2;");
    ExpectParseError(parser, "SELECT * FROM t WHERE name LIKE;");
    ExpectParseOk(parser, "SELECT * FROM t WHERE name LIKE \"a.*\";");

    // Aggregate form constraints.
    ExpectParseError(parser, "SELECT SUM(*), COUNT(*) FROM t;");
    ExpectParseError(parser, "SELECT AVG(*), COUNT(*) FROM t;");
    ExpectParseOk(parser, "SELECT COUNT(*), SUM(id), AVG(id) FROM t;");

    // REVERT forms (legacy short form is currently accepted by parser).
    ExpectParseOk(parser, "REVERT t;");
    ExpectParseOk(parser, "REVERT t EXACT x;");
    ExpectParseOk(parser, "REVERT t AT_OR_BEFORE x;");
    ExpectParseOk(parser, "REVERT t LATEST;");
    ExpectParseOk(parser, "REVERT t EXACT \"2026.05.20-10:00:00.000001\";");
    ExpectParseOk(parser,
                  "REVERT t AT_OR_BEFORE \"2026.05.20-10:00:00.000001\";");

    return 0;
}
