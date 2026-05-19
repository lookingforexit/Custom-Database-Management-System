#include <filesystem>
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
        std::cout << "tx_error: " << label << " code="
                  << static_cast<int>(result.error->code) << "\n";
        return false;
    }

} // namespace

int main() {
    const std::string root = "./test_data_transactions";
    std::filesystem::remove_all(root);

    dbms::core::DbmsEngine engine(root);
    dbms::core::SessionContext session_a;
    session_a.client_id = "A";
    dbms::core::SessionContext session_b;
    session_b.client_id = "B";

    if (!ExpectOk(engine.ExecuteSql(session_a, "CREATE DATABASE txdb;"),
                  "create_db")) {
        return 1;
    }
    if (!ExpectOk(engine.ExecuteSql(session_a, "USE txdb;"), "use_db_a")) {
        return 1;
    }
    if (!ExpectOk(engine.ExecuteSql(session_b, "USE txdb;"), "use_db_b")) {
        return 1;
    }
    if (!ExpectOk(engine.ExecuteSql(
                      session_a,
                      "CREATE TABLE t (id INT INDEXED, name STRING);"),
                  "create_table")) {
        return 1;
    }

    if (!ExpectOk(engine.ExecuteSql(session_a, "BEGIN;"), "begin_a")) {
        return 1;
    }
    if (!ExpectOk(engine.ExecuteSql(
                      session_a, "INSERT INTO t (id, name) VALUES (1, \"A\");"),
                  "insert_a_tx")) {
        return 1;
    }
    auto visible_to_a = engine.ExecuteSql(session_a, "SELECT * FROM t;");
    if (!visible_to_a.ok() || visible_to_a.value->rows.size() != 1) {
        std::cout << "tx_error: isolation_a_view\n";
        return 1;
    }
    auto hidden_from_b = engine.ExecuteSql(session_b, "SELECT * FROM t;");
    if (!hidden_from_b.ok() || !hidden_from_b.value->rows.empty()) {
        std::cout << "tx_error: isolation_b_view\n";
        return 1;
    }
    if (!ExpectOk(engine.ExecuteSql(session_a, "ROLLBACK;"), "rollback_a")) {
        return 1;
    }
    auto after_rollback = engine.ExecuteSql(session_a, "SELECT * FROM t;");
    if (!after_rollback.ok() || !after_rollback.value->rows.empty()) {
        std::cout << "tx_error: rollback_leak\n";
        return 1;
    }

    if (!ExpectOk(engine.ExecuteSql(session_a, "BEGIN;"), "begin_commit")) {
        return 1;
    }
    if (!ExpectOk(engine.ExecuteSql(
                      session_a, "INSERT INTO t (id, name) VALUES (2, \"B\");"),
                  "insert_commit")) {
        return 1;
    }
    if (!ExpectOk(engine.ExecuteSql(session_a, "COMMIT;"), "commit_a")) {
        return 1;
    }
    auto after_commit = engine.ExecuteSql(session_b, "SELECT * FROM t;");
    if (!after_commit.ok() || after_commit.value->rows.size() != 1) {
        std::cout << "tx_error: commit_visibility\n";
        return 1;
    }
    if (!std::holds_alternative<std::int64_t>(
            after_commit.value->rows[0].values[0]) ||
        std::get<std::int64_t>(after_commit.value->rows[0].values[0]) != 2) {
        std::cout << "tx_error: commit_value\n";
        return 1;
    }

    auto invalid_commit = engine.ExecuteSql(session_a, "COMMIT;");
    if (invalid_commit.ok() ||
        invalid_commit.error->code != dbms::common::ErrorCode::kValidationError) {
        std::cout << "tx_error: invalid_commit\n";
        return 1;
    }

    std::filesystem::remove_all(root);
    std::cout << "tx_ok\n";
    return 0;
}
