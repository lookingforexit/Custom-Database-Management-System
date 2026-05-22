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
        std::cout << "revert_persistence_error: " << label << " code="
                  << static_cast<int>(result.error->code) << "\n";
        return false;
    }

} // namespace

int main() {
    const std::string root = "./test_data_revert_persistence";
    std::filesystem::remove_all(root);

    std::string target_timestamp;
    {
        dbms::core::DbmsEngine engine(root);
        dbms::core::SessionContext session;
        session.client_id = "revert_persist_writer";

        if (!ExpectOk(engine.ExecuteSql(session, "CREATE DATABASE revp_db;"),
                      "create_db")) {
            return 1;
        }
        if (!ExpectOk(engine.ExecuteSql(session, "USE revp_db;"), "use_db")) {
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

        const auto history =
            engine.version_store().HistoryForTable("revp_db", "t");
        if (history.empty()) {
            std::cout << "revert_persistence_error: no_history\n";
            return 1;
        }
        target_timestamp = history.back().timestamp;

        if (!ExpectOk(engine.ExecuteSql(
                          session, "UPDATE t SET name = \"B\" WHERE id == 1;"),
                      "update_b")) {
            return 1;
        }
    }

    {
        dbms::core::DbmsEngine engine(root);
        dbms::core::SessionContext session;
        session.client_id = "revert_persist_reader";

        if (!ExpectOk(engine.ExecuteSql(session, "USE revp_db;"), "use_db_2")) {
            return 1;
        }
        if (!ExpectOk(engine.ExecuteSql(
                          session,
                          "REVERT t \"" + target_timestamp + "\";"),
                      "revert_after_restart")) {
            return 1;
        }

        auto after_revert =
            engine.ExecuteSql(session, "SELECT name FROM t WHERE id == 1;");
        if (!after_revert.ok() || after_revert.value->rows.size() != 1 ||
            dbms::common::GetValueType(after_revert.value->rows[0].values[0]) !=
                dbms::common::ValueType::kString ||
            dbms::common::AsString(after_revert.value->rows[0].values[0]) != "A") {
            std::cout << "revert_persistence_error: wrong_value_after_revert\n";
            return 1;
        }
    }

    std::filesystem::remove_all(root);
    std::cout << "revert_persistence_ok\n";
    return 0;
}
