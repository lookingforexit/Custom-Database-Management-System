#include <filesystem>
#include <string>

#include "common/types.hpp"
#include "core/dbms_engine.hpp"
#include "core/runtime_state.hpp"

int main() {
    const std::string root = "./test_data_index_reference";
    std::filesystem::remove_all(root);

    {
        dbms::core::DbmsEngine engine(root);
        dbms::core::SessionContext session;
        session.client_id = "idx_ref";

        if (!engine.ExecuteSql(session, "CREATE DATABASE idb;").ok()) return 1;
        if (!engine.ExecuteSql(session, "USE idb;").ok()) return 1;
        if (!engine.ExecuteSql(
                session,
                "CREATE TABLE users (id INT INDEXED, name STRING, score INT);")
                .ok()) {
            return 1;
        }
        if (!engine.ExecuteSql(
                session,
                "INSERT INTO users (id, name, score) VALUES (1, \"Ann\", 10), (2, \"Bob\", 20);")
                .ok()) {
            return 1;
        }

        // Index-based lookup should return current heap row payload.
        auto select_before =
            engine.ExecuteSql(session, "SELECT name, score FROM users WHERE id == 2;");
        if (!select_before.ok() || select_before.value->rows.size() != 1 ||
            !std::holds_alternative<std::string>(
                select_before.value->rows[0].values[0]) ||
            !std::holds_alternative<std::int64_t>(
                select_before.value->rows[0].values[1]) ||
            std::get<std::string>(select_before.value->rows[0].values[0]) != "Bob" ||
            std::get<std::int64_t>(select_before.value->rows[0].values[1]) != 20) {
            return 1;
        }

        // Update non-indexed payload columns. If index duplicated row payload,
        // stale values could appear; reference semantics must return updated heap data.
        if (!engine.ExecuteSql(
                session, "UPDATE users SET name = \"Bobby\", score = 99 WHERE id == 2;")
                .ok()) {
            return 1;
        }
        auto select_after =
            engine.ExecuteSql(session, "SELECT name, score FROM users WHERE id == 2;");
        if (!select_after.ok() || select_after.value->rows.size() != 1 ||
            std::get<std::string>(select_after.value->rows[0].values[0]) != "Bobby" ||
            std::get<std::int64_t>(select_after.value->rows[0].values[1]) != 99) {
            return 1;
        }

        // Runtime consistency checker must pass (index row_id references heap row).
        auto runtime_check =
            dbms::core::ValidateRuntimeIndexConsistency(engine.runtime_state());
        if (!runtime_check.ok()) return 1;
    }

    // After restart, index must still reference restored heap rows.
    {
        dbms::core::DbmsEngine engine(root);
        dbms::core::SessionContext session;
        session.client_id = "idx_ref_after_restart";
        if (!engine.ExecuteSql(session, "USE idb;").ok()) return 1;

        auto select_after_restart =
            engine.ExecuteSql(session, "SELECT name, score FROM users WHERE id == 2;");
        if (!select_after_restart.ok() || select_after_restart.value->rows.size() != 1 ||
            std::get<std::string>(select_after_restart.value->rows[0].values[0]) !=
                "Bobby" ||
            std::get<std::int64_t>(select_after_restart.value->rows[0].values[1]) !=
                99) {
            return 1;
        }
        auto runtime_check =
            dbms::core::ValidateRuntimeIndexConsistency(engine.runtime_state());
        if (!runtime_check.ok()) return 1;
    }

    std::filesystem::remove_all(root);
    return 0;
}
