#include <filesystem>
#include <string>

#include "core/dbms_engine.hpp"

int main() {
    const std::string root = "./test_data_revert_tx";
    std::filesystem::remove_all(root);

    dbms::core::DbmsEngine engine(root);
    dbms::core::SessionContext session_a;
    session_a.client_id = "A";
    dbms::core::SessionContext session_b;
    session_b.client_id = "B";

    if (!engine.ExecuteSql(session_a, "CREATE DATABASE rtx;").ok()) return 1;
    if (!engine.ExecuteSql(session_a, "USE rtx;").ok()) return 1;
    if (!engine.ExecuteSql(session_b, "USE rtx;").ok()) return 1;
    if (!engine.ExecuteSql(
             session_a, "CREATE TABLE t (id INT INDEXED, name STRING);")
             .ok()) {
        return 1;
    }
    if (!engine.ExecuteSql(session_a,
                           "INSERT INTO t (id, name) VALUES (1, \"A\");")
             .ok()) {
        return 1;
    }
    const auto history = engine.version_store().HistoryForTable("rtx", "t");
    if (history.empty()) return 1;
    const std::string ts_a = history.back().timestamp;
    if (!engine.ExecuteSql(session_a,
                           "UPDATE t SET name = \"B\" WHERE id == 1;")
             .ok()) {
        return 1;
    }

    // REVERT inside tx is isolated and rollbackable.
    if (!engine.ExecuteSql(session_a, "BEGIN;").ok()) return 1;
    if (!engine.ExecuteSql(session_a, "REVERT t EXACT \"" + ts_a + "\";").ok()) {
        return 1;
    }
    auto a_view = engine.ExecuteSql(session_a, "SELECT name FROM t WHERE id == 1;");
    auto b_view = engine.ExecuteSql(session_b, "SELECT name FROM t WHERE id == 1;");
    if (!a_view.ok() || !b_view.ok()) return 1;
    if (dbms::common::AsString(a_view.value->rows[0].values[0]) != "A") return 1;
    if (dbms::common::AsString(b_view.value->rows[0].values[0]) != "B") return 1;
    if (!engine.ExecuteSql(session_a, "ROLLBACK;").ok()) return 1;
    auto after_rollback =
        engine.ExecuteSql(session_a, "SELECT name FROM t WHERE id == 1;");
    if (!after_rollback.ok()) return 1;
    if (dbms::common::AsString(after_rollback.value->rows[0].values[0]) != "B") {
        return 1;
    }

    // REVERT inside tx on commit becomes visible globally.
    if (!engine.ExecuteSql(session_a, "BEGIN;").ok()) return 1;
    if (!engine.ExecuteSql(session_a, "REVERT t EXACT \"" + ts_a + "\";").ok()) {
        return 1;
    }
    if (!engine.ExecuteSql(session_a, "COMMIT;").ok()) return 1;
    auto after_commit_b =
        engine.ExecuteSql(session_b, "SELECT name FROM t WHERE id == 1;");
    if (!after_commit_b.ok()) return 1;
    if (dbms::common::AsString(after_commit_b.value->rows[0].values[0]) != "A") {
        return 1;
    }

    std::filesystem::remove_all(root);
    return 0;
}
