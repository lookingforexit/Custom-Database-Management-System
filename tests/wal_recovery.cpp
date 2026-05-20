#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "core/dbms_engine.hpp"
#include "core/wal_manager.hpp"

int main() {
    const std::string root = "./test_data_wal_recovery";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);

    dbms::core::WalManager wal(root);
    if (!wal.Reset()) return 1;
    if (!wal.AppendSql("CREATE DATABASE wdb;")) return 1;
    if (!wal.AppendSql("USE wdb;")) return 1;
    if (!wal.AppendSql("CREATE TABLE t (id INT INDEXED, name STRING);")) return 1;
    if (!wal.AppendTransaction({
            "INSERT INTO t (id, name) VALUES (1, \"A\");",
            "INSERT INTO t (id, name) VALUES (2, \"B\");",
        })) {
        return 1;
    }

    dbms::core::DbmsEngine engine(root);
    dbms::core::SessionContext session;
    session.client_id = "wal_reader";
    if (!engine.ExecuteSql(session, "USE wdb;").ok()) return 1;
    auto select_all = engine.ExecuteSql(session, "SELECT COUNT(*) FROM t;");
    if (!select_all.ok() || select_all.value->rows.size() != 1 ||
        !std::holds_alternative<std::int64_t>(select_all.value->rows[0].values[0]) ||
        std::get<std::int64_t>(select_all.value->rows[0].values[0]) != 2) {
        return 1;
    }

    std::vector<dbms::core::WalEntry> remaining;
    if (!wal.LoadAll(remaining)) return 1;
    if (!remaining.empty()) return 1;

    // Idempotency across restart after checkpointed WAL.
    dbms::core::DbmsEngine engine_restarted(root);
    dbms::core::SessionContext session2;
    session2.client_id = "wal_reader_2";
    if (!engine_restarted.ExecuteSql(session2, "USE wdb;").ok()) return 1;
    auto count_again = engine_restarted.ExecuteSql(session2, "SELECT COUNT(*) FROM t;");
    if (!count_again.ok() || count_again.value->rows.size() != 1 ||
        !std::holds_alternative<std::int64_t>(count_again.value->rows[0].values[0]) ||
        std::get<std::int64_t>(count_again.value->rows[0].values[0]) != 2) {
        return 1;
    }

    std::filesystem::remove_all(root);

    // Undo: incomplete TX in WAL must be ignored.
    {
        const std::string root_undo = "./test_data_wal_undo";
        std::filesystem::remove_all(root_undo);
        dbms::core::WalManager wal_undo(root_undo);
        if (!wal_undo.Reset()) return 1;
        if (!wal_undo.AppendSql("CREATE DATABASE udb;")) return 1;
        if (!wal_undo.AppendSql("USE udb;")) return 1;
        if (!wal_undo.AppendSql("CREATE TABLE t (id INT INDEXED, name STRING);")) {
            return 1;
        }
        {
            std::ofstream out(root_undo + "/wal.log", std::ios::app);
            out << "TX_BEGIN\n";
            out << "TX_SQL\tINSERT INTO t (id, name) VALUES (1, \"X\");\n";
            // no TX_END => uncommitted tail
        }
        dbms::core::DbmsEngine engine_undo(root_undo);
        dbms::core::SessionContext session;
        session.client_id = "wal_undo_reader";
        if (!engine_undo.ExecuteSql(session, "USE udb;").ok()) return 1;
        auto count = engine_undo.ExecuteSql(session, "SELECT COUNT(*) FROM t;");
        if (!count.ok() || count.value->rows.size() != 1 ||
            !std::holds_alternative<std::int64_t>(count.value->rows[0].values[0]) ||
            std::get<std::int64_t>(count.value->rows[0].values[0]) != 0) {
            return 1;
        }
        std::filesystem::remove_all(root_undo);
    }

    // Corrupted WAL must be quarantined and startup should continue from persisted state.
    {
        const std::string root_corrupt = "./test_data_wal_corrupt";
        std::filesystem::remove_all(root_corrupt);
        {
            dbms::core::DbmsEngine engine(root_corrupt);
            dbms::core::SessionContext session;
            session.client_id = "wal_corrupt_writer";
            if (!engine.ExecuteSql(session, "CREATE DATABASE cdb;").ok()) return 1;
            if (!engine.ExecuteSql(session, "USE cdb;").ok()) return 1;
            if (!engine.ExecuteSql(session,
                                   "CREATE TABLE t (id INT INDEXED, name STRING);")
                     .ok()) {
                return 1;
            }
            if (!engine.ExecuteSql(session,
                                   "INSERT INTO t (id, name) VALUES (1, \"A\");")
                     .ok()) {
                return 1;
            }
        }
        {
            std::ofstream out(root_corrupt + "/wal.log", std::ios::trunc);
            out << "WAL\t1\n";
            out << "BROKEN_LINE\n";
        }
        dbms::core::DbmsEngine engine_after(root_corrupt);
        dbms::core::SessionContext session_after;
        session_after.client_id = "wal_corrupt_reader";
        if (!engine_after.ExecuteSql(session_after, "USE cdb;").ok()) return 1;
        auto count_after =
            engine_after.ExecuteSql(session_after, "SELECT COUNT(*) FROM t;");
        if (!count_after.ok() || count_after.value->rows.size() != 1 ||
            !std::holds_alternative<std::int64_t>(
                count_after.value->rows[0].values[0]) ||
            std::get<std::int64_t>(count_after.value->rows[0].values[0]) != 1) {
            return 1;
        }
        bool has_quarantine = false;
        for (const auto &entry : std::filesystem::directory_iterator(root_corrupt)) {
            if (!entry.is_regular_file()) {
                continue;
            }
            const auto name = entry.path().filename().string();
            if (name.rfind("wal.log.corrupt.", 0) == 0) {
                has_quarantine = true;
                break;
            }
        }
        if (!has_quarantine) return 1;
        std::filesystem::remove_all(root_corrupt);
    }

    // WAL format version compatibility: v1 is supported.
    {
        const std::string root_v1 = "./test_data_wal_v1";
        std::filesystem::remove_all(root_v1);
        std::filesystem::create_directories(root_v1);
        {
            std::ofstream out(root_v1 + "/wal.log", std::ios::trunc);
            out << "WAL\t1\n";
            out << "SQL\tCREATE DATABASE vdb;\n";
            out << "SQL\tUSE vdb;\n";
            out << "SQL\tCREATE TABLE t (id INT INDEXED, name STRING);\n";
            out << "SQL\tINSERT INTO t (id, name) VALUES (1, \"A\");\n";
        }
        dbms::core::DbmsEngine engine_v1(root_v1);
        dbms::core::SessionContext session_v1;
        session_v1.client_id = "wal_v1_reader";
        if (!engine_v1.ExecuteSql(session_v1, "USE vdb;").ok()) return 1;
        auto count_v1 = engine_v1.ExecuteSql(session_v1, "SELECT COUNT(*) FROM t;");
        if (!count_v1.ok() || count_v1.value->rows.size() != 1 ||
            !std::holds_alternative<std::int64_t>(count_v1.value->rows[0].values[0]) ||
            std::get<std::int64_t>(count_v1.value->rows[0].values[0]) != 1) {
            return 1;
        }
        std::filesystem::remove_all(root_v1);
    }

    // Unsupported WAL version must be quarantined.
    {
        const std::string root_bad_ver = "./test_data_wal_bad_version";
        std::filesystem::remove_all(root_bad_ver);
        std::filesystem::create_directories(root_bad_ver);
        {
            std::ofstream out(root_bad_ver + "/wal.log", std::ios::trunc);
            out << "WAL\t999\n";
            out << "SQL\tCREATE DATABASE bad;\n";
        }
        dbms::core::DbmsEngine engine_bad_ver(root_bad_ver);
        (void)engine_bad_ver;
        bool has_quarantine = false;
        for (const auto &entry : std::filesystem::directory_iterator(root_bad_ver)) {
            if (!entry.is_regular_file()) continue;
            const auto name = entry.path().filename().string();
            if (name.rfind("wal.log.corrupt.", 0) == 0) {
                has_quarantine = true;
                break;
            }
        }
        if (!has_quarantine) return 1;
        std::filesystem::remove_all(root_bad_ver);
    }

    return 0;
}
