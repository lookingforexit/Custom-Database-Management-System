#include "common/error.hpp"
#include "core/dbms_engine.hpp"

#include <filesystem>
#include <string>

namespace {

    bool ExpectOk(const dbms::common::Result<dbms::execution::QueryResult> &result) {
        return result.ok();
    }

    bool ExpectCount(dbms::core::DbmsEngine &engine, dbms::core::SessionContext &session,
                     const std::string &sql, std::int64_t expected) {
        auto result = engine.ExecuteSql(session, sql);
        if (!result.ok() || result.value->rows.size() != 1 ||
            result.value->rows[0].values.size() != 1 ||
            !std::holds_alternative<std::int64_t>(result.value->rows[0].values[0])) {
            return false;
        }
        return std::get<std::int64_t>(result.value->rows[0].values[0]) == expected;
    }

} // namespace

int main() {
    const std::string root = "./test_data_where_and_or_stress";
    std::filesystem::remove_all(root);

    dbms::core::DbmsEngine engine(root);
    dbms::core::SessionContext session;
    session.client_id = "where_and_or_stress";

    if (!ExpectOk(engine.ExecuteSql(session, "CREATE DATABASE wdb;"))) return 1;
    if (!ExpectOk(engine.ExecuteSql(session, "USE wdb;"))) return 1;
    if (!ExpectOk(engine.ExecuteSql(
            session,
            "CREATE TABLE t (id INT INDEXED, a INT, b INT, tag STRING DEFAULT \"x\");"))) {
        return 1;
    }

    if (!ExpectOk(engine.ExecuteSql(
            session,
            "INSERT INTO t (id, a, b, tag) VALUES "
            "(1, 1, 10, \"alpha\"),"
            "(2, 1, 20, \"beta\"),"
            "(3, 2, 10, \"alpha\"),"
            "(4, 2, 20, \"beta\"),"
            "(5, 3, 30, \"gamma\"),"
            "(6, 3, 10, \"alpha\"),"
            "(7, 4, 40, \"delta\"),"
            "(8, 4, 20, \"beta\");"))) {
        return 1;
    }

    // precedence: AND binds stronger than OR
    if (!ExpectCount(engine, session,
                     "SELECT COUNT(*) FROM t WHERE a == 1 OR a == 2 AND b == 20;",
                     3)) {
        return 1;
    }
    if (!ExpectCount(engine, session,
                     "SELECT COUNT(*) FROM t WHERE (a == 1 OR a == 2) AND b == 20;",
                     2)) {
        return 1;
    }

    // nested parenthesis and mixed comparisons
    if (!ExpectCount(engine, session,
                     "SELECT COUNT(*) FROM t WHERE ((a >= 2 AND b <= 20) OR (a == 4 AND b == 40));",
                     5)) {
        return 1;
    }

    // OR over disjoint predicates
    if (!ExpectCount(engine, session,
                     "SELECT COUNT(*) FROM t WHERE (a == 3 AND b == 30) OR (a == 4 AND b == 20);",
                     2)) {
        return 1;
    }

    // LIKE + AND/OR combination
    if (!ExpectCount(engine, session,
                     "SELECT COUNT(*) FROM t WHERE (tag LIKE \"a.*\" OR tag LIKE \"b.*\") AND b == 20;",
                     3)) {
        return 1;
    }

    // BETWEEN + OR
    if (!ExpectCount(engine, session,
                     "SELECT COUNT(*) FROM t WHERE a BETWEEN 2 AND 4 OR id == 1;",
                     5)) {
        return 1;
    }

    // unsatisfiable branch must not affect other OR branch
    if (!ExpectCount(engine, session,
                     "SELECT COUNT(*) FROM t WHERE (a == 999 AND b == 999) OR (a == 1 AND b == 10);",
                     1)) {
        return 1;
    }

    // stress chain with many OR terms
    if (!ExpectCount(engine, session,
                     "SELECT COUNT(*) FROM t WHERE id == 1 OR id == 2 OR id == 3 OR id == 4 OR id == 5 OR id == 6 OR id == 7 OR id == 8;",
                     8)) {
        return 1;
    }

    std::filesystem::remove_all(root);
    return 0;
}
