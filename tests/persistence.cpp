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
        std::cout << "persistence_error: " << label << " failed with code "
                  << static_cast<int>(result.error->code) << "\n";
        return false;
    }

} // namespace

int main() {
    const std::string root = "./test_data_persistence";
    std::filesystem::remove_all(root);

    {
        dbms::core::DbmsEngine engine(root);
        dbms::core::SessionContext session;
        session.client_id = "persist_writer";

        if (!ExpectOk(engine.ExecuteSql(session, "CREATE DATABASE persist_db;"),
                      "create_db")) {
            return 1;
        }
        if (!ExpectOk(engine.ExecuteSql(session, "USE persist_db;"), "use_db")) {
            return 1;
        }
        if (!ExpectOk(engine.ExecuteSql(
                          session,
                          "CREATE TABLE users (id INT INDEXED, age INT, name STRING);"),
                      "create_table")) {
            return 1;
        }
        if (!ExpectOk(engine.ExecuteSql(
                          session,
                          "INSERT INTO users (id, age, name) VALUES (1, 18, \"Ann\"), (2, 20, \"Bob\"), (3, 25, \"Cara\");"),
                      "insert")) {
            return 1;
        }
        if (!ExpectOk(engine.ExecuteSql(
                          session, "UPDATE users SET age = 30 WHERE id == 2;"),
                      "update")) {
            return 1;
        }
        if (!ExpectOk(engine.ExecuteSql(session, "DELETE FROM users WHERE id == 1;"),
                      "delete")) {
            return 1;
        }
    }

    {
        dbms::core::DbmsEngine engine(root);
        dbms::core::SessionContext session;
        session.client_id = "persist_reader";

        if (!ExpectOk(engine.ExecuteSql(session, "USE persist_db;"), "use_db_2")) {
            return 1;
        }

        auto count_result =
            engine.ExecuteSql(session, "SELECT COUNT(*) FROM users;");
        if (!count_result.ok() || count_result.value->rows.size() != 1 ||
            count_result.value->rows[0].values.size() != 1 ||
            !std::holds_alternative<std::int64_t>(
                count_result.value->rows[0].values[0]) ||
            std::get<std::int64_t>(count_result.value->rows[0].values[0]) != 2) {
            std::cout << "persistence_error: count_mismatch\n";
            return 1;
        }

        auto indexed_select =
            engine.ExecuteSql(session, "SELECT * FROM users WHERE id == 2;");
        if (!indexed_select.ok() || indexed_select.value->rows.size() != 1 ||
            indexed_select.value->rows[0].values.size() != 3 ||
            !std::holds_alternative<std::int64_t>(
                indexed_select.value->rows[0].values[0]) ||
            std::get<std::int64_t>(indexed_select.value->rows[0].values[0]) != 2 ||
            !std::holds_alternative<std::int64_t>(
                indexed_select.value->rows[0].values[1]) ||
            std::get<std::int64_t>(indexed_select.value->rows[0].values[1]) != 30 ||
            dbms::common::GetValueType(indexed_select.value->rows[0].values[2]) !=
                dbms::common::ValueType::kString ||
            dbms::common::AsString(indexed_select.value->rows[0].values[2]) != "Bob") {
            std::cout << "persistence_error: indexed_select_mismatch\n";
            if (indexed_select.ok()) {
                std::cout << "row_count=" << indexed_select.value->rows.size()
                          << "\n";
                if (!indexed_select.value->rows.empty()) {
                    std::cout << "value_count="
                              << indexed_select.value->rows[0].values.size()
                              << "\n";
                    for (const auto &value : indexed_select.value->rows[0].values) {
                        std::cout << "value=" << dbms::common::ValueToString(value)
                                  << "\n";
                    }
                }
            }
            return 1;
        }

        // Additional edge checks on recovered state.
        auto missing_row =
            engine.ExecuteSql(session, "SELECT * FROM users WHERE id == 999;");
        if (!missing_row.ok() || !missing_row.value->rows.empty()) {
            std::cout << "persistence_error: missing_row_not_empty\n";
            return 1;
        }

        auto count_non_null_age =
            engine.ExecuteSql(session, "SELECT COUNT(age) FROM users;");
        if (!count_non_null_age.ok() || count_non_null_age.value->rows.size() != 1 ||
            !std::holds_alternative<std::int64_t>(
                count_non_null_age.value->rows[0].values[0]) ||
            std::get<std::int64_t>(count_non_null_age.value->rows[0].values[0]) !=
                2) {
            std::cout << "persistence_error: count_age_mismatch\n";
            return 1;
        }
    }

    // Recovery idempotency: repeated restarts must yield identical answers.
    for (int restart = 0; restart < 3; ++restart) {
        dbms::core::DbmsEngine engine(root);
        dbms::core::SessionContext session;
        session.client_id = "persist_reader_restart_" + std::to_string(restart);
        if (!ExpectOk(engine.ExecuteSql(session, "USE persist_db;"),
                      "use_db_restart")) {
            return 1;
        }
        auto stable = engine.ExecuteSql(
            session, "SELECT COUNT(*), SUM(age), AVG(age) FROM users;");
        if (!stable.ok() || stable.value->rows.size() != 1 ||
            stable.value->rows[0].values.size() != 3) {
            std::cout << "persistence_error: restart_query_failed\n";
            return 1;
        }
        if (!std::holds_alternative<std::int64_t>(stable.value->rows[0].values[0]) ||
            !std::holds_alternative<std::int64_t>(stable.value->rows[0].values[1]) ||
            !std::holds_alternative<std::int64_t>(stable.value->rows[0].values[2]) ||
            std::get<std::int64_t>(stable.value->rows[0].values[0]) != 2 ||
            std::get<std::int64_t>(stable.value->rows[0].values[1]) != 55 ||
            std::get<std::int64_t>(stable.value->rows[0].values[2]) != 27) {
            std::cout << "persistence_error: restart_aggregate_mismatch\n";
            return 1;
        }
    }

    std::filesystem::remove_all(root);
    std::cout << "persistence_ok\n";
    return 0;
}
