#include <filesystem>
#include <string>

#include "core/dbms_engine.hpp"
#include "core/runtime_state.hpp"

int main() {
    const std::string root = "./test_data_index_consistency";
    std::filesystem::remove_all(root);

    {
        dbms::core::DbmsEngine engine(root);
        dbms::core::SessionContext session;
        session.client_id = "idx";

        if (!engine.ExecuteSql(session, "CREATE DATABASE idxdb;").ok()) return 1;
        if (!engine.ExecuteSql(session, "USE idxdb;").ok()) return 1;
        if (!engine.ExecuteSql(
                 session,
                 "CREATE TABLE t (id INT INDEXED, name STRING, score INT);")
                 .ok()) {
            return 1;
        }
        if (!engine.ExecuteSql(
                 session,
                 "INSERT INTO t (id, name, score) VALUES (1, \"A\", 10), (2, \"B\", 20), (3, \"C\", 30);")
                 .ok()) {
            return 1;
        }
        if (!engine.ExecuteSql(
                 session, "UPDATE t SET score = 25 WHERE id == 2;")
                 .ok()) {
            return 1;
        }
        if (!engine.ExecuteSql(session, "DELETE FROM t WHERE id == 1;").ok()) {
            return 1;
        }
        const auto history = engine.version_store().HistoryForTable("idxdb", "t");
        if (history.empty()) return 1;
        const std::string ts = history.front().timestamp;
        if (!engine.ExecuteSql(session, "REVERT t AT_OR_BEFORE \"" + ts + "\";")
                 .ok()) {
            return 1;
        }
        auto validation =
            dbms::core::ValidateRuntimeIndexConsistency(engine.runtime_state());
        if (!validation.ok()) return 1;
    }

    {
        dbms::core::DbmsEngine engine(root);
        dbms::core::SessionContext session;
        session.client_id = "idx2";
        if (!engine.ExecuteSql(session, "USE idxdb;").ok()) return 1;
        auto validation =
            dbms::core::ValidateRuntimeIndexConsistency(engine.runtime_state());
        if (!validation.ok()) return 1;
        auto select =
            engine.ExecuteSql(session, "SELECT id FROM t WHERE id BETWEEN 1 AND 4;");
        if (!select.ok()) return 1;
    }

    std::filesystem::remove_all(root);
    return 0;
}
