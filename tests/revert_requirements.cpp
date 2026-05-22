#include <cassert>
#include <filesystem>
#include <fstream>
#include <sstream>
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

    // Persisted history must be event-based and must not contain snapshot dumps.
    {
        const std::string root = "./test_data_revert_history_audit";
        std::filesystem::remove_all(root);

        dbms::core::DbmsEngine engine(root);
        dbms::core::SessionContext session;
        session.client_id = "revert_history_audit";

        assert(engine.ExecuteSql(session, "CREATE DATABASE audit;").ok());
        assert(engine.ExecuteSql(session, "USE audit;").ok());
        assert(engine.ExecuteSql(
                          session,
                          "CREATE TABLE t (id INT INDEXED, name STRING);")
                   .ok());
        for (int index = 1; index <= 4; ++index) {
            assert(engine.ExecuteSql(
                              session,
                              "INSERT INTO t (id, name) VALUES (" +
                                  std::to_string(index) + ", \"same\");")
                       .ok());
        }
        assert(engine.ExecuteSql(
                          session, "UPDATE t SET name = \"other\" WHERE id == 2;")
                   .ok());
        assert(engine.ExecuteSql(session, "DELETE FROM t WHERE id == 3;").ok());

        std::ifstream history(root + "/version_history.tsv");
        assert(history.is_open());
        std::stringstream buffer;
        buffer << history.rdbuf();
        const std::string content = buffer.str();
        assert(content.find("FORMAT\t3") != std::string::npos);
        assert(content.find("EVENT\tkind=INSERT_ROW") != std::string::npos);
        assert(content.find("EVENT\tkind=UPDATE_ROW") != std::string::npos);
        assert(content.find("EVENT\tkind=DELETE_ROW") != std::string::npos);
        assert(content.find("BEFORE\t") != std::string::npos);
        assert(content.find("AFTER\t") != std::string::npos);
        assert(content.find("SNAPSHOT_ROW") == std::string::npos);
        assert(content.find("snapshot_rows") == std::string::npos);

        std::filesystem::remove_all(root);
    }

    // Boundary checks for dense timestamps.
    {
        dbms::versioning::VersionStore store;
        const auto t1 = store.Append({.kind = dbms::versioning::ChangeKind::kInsertRow,
                                      .database_name = "db",
                                      .table_name = "t",
                                      .timestamp = "2026.05.20-12:00:00.000001"});
        const auto t2 = store.Append({.kind = dbms::versioning::ChangeKind::kUpdateRow,
                                      .database_name = "db",
                                      .table_name = "t",
                                      .timestamp = "2026.05.20-12:00:00.000002"});
        assert(!t1.empty());
        assert(!t2.empty());

        assert(store.HasExactTimestampForTable(
            "db", "t", "2026.05.20-12:00:00.000001"));
        assert(store.LatestTimestampForTable("db", "t").has_value());
        assert(*store.LatestTimestampForTable("db", "t") ==
               "2026.05.20-12:00:00.000002");

        auto at_or_before =
            store.LatestTimestampAtOrBefore("db", "t",
                                            "2026.05.20-12:00:00.000001");
        assert(at_or_before.has_value());
        assert(*at_or_before == "2026.05.20-12:00:00.000001");
        assert(!store.LatestTimestampAtOrBefore(
                         "db", "t", "2026.05.20-11:59:59.999999")
                    .has_value());
    }

    // Corrupted/incompatible version history should fail loading.
    {
        const std::string root = "./test_data_revert_corrupt";
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);

        {
            std::ofstream state(root + "/runtime_state.tsv", std::ios::trunc);
            state << "FORMAT\t3\n";
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
            history << "FORMAT\t3\n";
            history << "EVENT\tkind=INSERT_ROW\tdb=x\ttable=t\ttimestamp=2026.05.20-12:00:00.000001\trow_id=1\n";
            history << "AFTER\trow_id=1\tvalue=Int:1\tvalue=String:A\n";
            // Missing EVENT_END -> malformed.
        }
        assert(!persistence.Load(runtime_state, version_store, string_pool));

        std::filesystem::remove_all(root);
    }

    return 0;
}
