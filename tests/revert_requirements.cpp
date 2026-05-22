#include <cassert>
#include <filesystem>
#include <fstream>
#include <string>

#include "common/error.hpp"
#include "core/dbms_engine.hpp"
#include "core/runtime_persistence.hpp"
#include "core/runtime_state.hpp"
#include "versioning/version_store.hpp"

int main() {
    {
        const std::string root = "./test_data_revert_requirements";
        std::filesystem::remove_all(root);
        dbms::core::DbmsEngine engine(root);
        dbms::core::SessionContext session;
        session.client_id = "revert_requirements";

        assert(engine.ExecuteSql(session, "CREATE DATABASE r1;").ok());
        assert(engine.ExecuteSql(session, "USE r1;").ok());
        assert(engine.ExecuteSql(session, "CREATE TABLE t (id INT INDEXED, name STRING);")
                   .ok());
        assert(engine.ExecuteSql(session, "INSERT INTO t (id, name) VALUES (1, \"A\");")
                   .ok());

        const auto history = engine.version_store().HistoryForTable("r1", "t");
        assert(!history.empty());
        const std::string ts_insert = history.back().timestamp;

        assert(engine.ExecuteSql(session, "UPDATE t SET name = \"B\" WHERE id == 1;")
                   .ok());

        auto exact_revert = engine.ExecuteSql(
            session, "REVERT t EXACT \"" + ts_insert + "\";");
        assert(exact_revert.ok());
        auto value = engine.ExecuteSql(session, "SELECT name FROM t WHERE id == 1;");
        assert(value.ok());
        assert(value.value->rows.size() == 1);
        assert(dbms::common::AsString(value.value->rows[0].values[0]) == "A");

        auto invalid_timestamp =
            engine.ExecuteSql(session, "REVERT t EXACT \"bad-timestamp\";");
        assert(!invalid_timestamp.ok());
        assert(invalid_timestamp.error->code ==
               dbms::common::ErrorCode::kValidationError);

        auto exact_not_found = engine.ExecuteSql(
            session, "REVERT t EXACT \"2001.01.01-00:00:00.000000\";");
        assert(!exact_not_found.ok());
        assert(exact_not_found.error->code == dbms::common::ErrorCode::kNotFound);

        auto at_or_before_not_found = engine.ExecuteSql(
            session, "REVERT t AT_OR_BEFORE \"2001.01.01-00:00:00.000000\";");
        assert(!at_or_before_not_found.ok());
        assert(at_or_before_not_found.error->code ==
               dbms::common::ErrorCode::kNotFound);

        auto latest_revert = engine.ExecuteSql(session, "REVERT t LATEST;");
        assert(latest_revert.ok());

        std::filesystem::remove_all(root);
    }

    // Boundary checks for dense timestamps.
    {
        dbms::versioning::VersionStore store;
        const auto t1 = store.Append({.database_name = "db",
                                      .table_name = "t",
                                      .operation = "X",
                                      .timestamp = "2026.05.20-12:00:00.000001",
                                      .snapshot_rows = {}});
        const auto t2 = store.Append({.database_name = "db",
                                      .table_name = "t",
                                      .operation = "Y",
                                      .timestamp = "2026.05.20-12:00:00.000002",
                                      .snapshot_rows = {}});
        assert(!t1.empty());
        assert(!t2.empty());

        auto exact = store.SnapshotExact("db", "t", "2026.05.20-12:00:00.000001");
        assert(exact.has_value());
        assert(exact->operation == "X");

        auto at_or_before =
            store.SnapshotAtOrBefore("db", "t", "2026.05.20-12:00:00.000001");
        assert(at_or_before.has_value());
        assert(at_or_before->operation == "X");
    }

    // Corrupted/incompatible version history should fail loading.
    {
        const std::string root = "./test_data_revert_corrupt";
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);

        {
            std::ofstream state(root + "/runtime_state.tsv", std::ios::trunc);
            state << "FORMAT\t2\n";
        }
        {
            std::ofstream history(root + "/version_history.tsv", std::ios::trunc);
            history << "FORMAT\t999\n";
        }

        dbms::core::RuntimeState runtime_state;
        dbms::versioning::VersionStore version_store;
        dbms::storage::StringPool string_pool;
        dbms::core::RuntimePersistence persistence(root);
        assert(!persistence.Load(runtime_state, version_store, string_pool));

        {
            std::ofstream history(root + "/version_history.tsv", std::ios::trunc);
            history << "FORMAT\t2\n";
            history << "CHANGE\tdb\tt\tINSERT\t2026.05.20-12:00:00.000001\n";
            history << "SNAPSHOT_ROW\tI:1\tS:A\n";
            // Missing CHANGE_END -> malformed.
        }
        assert(!persistence.Load(runtime_state, version_store, string_pool));

        std::filesystem::remove_all(root);
    }

    return 0;
}
