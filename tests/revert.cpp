#include <iostream>
#include <string>

#include "common/error.hpp"
#include "common/types.hpp"
#include "core/dbms_engine.hpp"

namespace {

    bool ExpectOk(const dbms::common::Result<dbms::execution::QueryResult> &result,
                  const std::string &label) {
        if (result.ok()) {
            return true;
        }
        std::cout << "revert_error: " << label << " code="
                  << static_cast<int>(result.error->code) << "\n";
        return false;
    }

} // namespace

int main() {
    dbms::core::DbmsEngine engine("./test_data_revert");
    dbms::core::SessionContext session;
    session.client_id = "revert";

    auto drop_result = engine.ExecuteSql(session, "DROP DATABASE rev_db;");
    if (!drop_result.ok() &&
        drop_result.error->code != dbms::common::ErrorCode::kNotFound) {
        std::cout << "revert_error: drop_db\n";
        return 1;
    }

    if (!ExpectOk(engine.ExecuteSql(session, "CREATE DATABASE rev_db;"),
                  "create_db")) {
        return 1;
    }
    if (!ExpectOk(engine.ExecuteSql(session, "USE rev_db;"), "use_db")) {
        return 1;
    }
    if (!ExpectOk(engine.ExecuteSql(
                      session,
                      "CREATE TABLE t (id INT INDEXED, name STRING);"),
                  "create_table")) {
        return 1;
    }
    if (!ExpectOk(engine.ExecuteSql(
                      session, "INSERT INTO t (id, name) VALUES (1, \"A\");"),
                  "insert_a")) {
        return 1;
    }

    const auto history_after_insert = engine.version_store().HistoryForTable(
        "rev_db", "t");
    if (history_after_insert.empty()) {
        std::cout << "revert_error: no_history\n";
        return 1;
    }
    const std::string restore_timestamp = history_after_insert.back().timestamp;

    if (!ExpectOk(engine.ExecuteSql(session, "UPDATE t SET name = \"B\" WHERE id == 1;"),
                  "update_b")) {
        return 1;
    }

    auto before_revert = engine.ExecuteSql(session, "SELECT name FROM t WHERE id == 1;");
    if (!before_revert.ok() || before_revert.value->rows.size() != 1 ||
        !std::holds_alternative<std::string>(before_revert.value->rows[0].values[0]) ||
        std::get<std::string>(before_revert.value->rows[0].values[0]) != "B") {
        std::cout << "revert_error: before_revert_value\n";
        return 1;
    }

    if (!ExpectOk(engine.ExecuteSql(session, "REVERT t \"" + restore_timestamp + "\";"),
                  "revert")) {
        return 1;
    }

    auto after_revert = engine.ExecuteSql(session, "SELECT name FROM t WHERE id == 1;");
    if (!after_revert.ok() || after_revert.value->rows.size() != 1 ||
        !std::holds_alternative<std::string>(after_revert.value->rows[0].values[0]) ||
        std::get<std::string>(after_revert.value->rows[0].values[0]) != "A") {
        std::cout << "revert_error: after_revert_value\n";
        return 1;
    }

    std::cout << "revert_ok\n";
    return 0;
}
