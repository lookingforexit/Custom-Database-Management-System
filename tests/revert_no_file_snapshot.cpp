#include <filesystem>
#include <string>

#include "core/dbms_engine.hpp"

int main() {
    const std::string root = "./test_data_revert_audit";
    std::filesystem::remove_all(root);

    dbms::core::DbmsEngine engine(root);
    dbms::core::SessionContext session;
    session.client_id = "revert_audit";

    if (!engine.ExecuteSql(session, "CREATE DATABASE aud;").ok()) return 1;
    if (!engine.ExecuteSql(session, "USE aud;").ok()) return 1;
    if (!engine.ExecuteSql(session, "CREATE TABLE t (id INT INDEXED, name STRING);")
             .ok()) {
        return 1;
    }
    if (!engine.ExecuteSql(session,
                           "INSERT INTO t (id, name) VALUES (1, \"A\"), (2, \"B\");")
             .ok()) {
        return 1;
    }
    if (!engine.ExecuteSql(session, "UPDATE t SET name = \"C\" WHERE id == 2;")
             .ok()) {
        return 1;
    }

    const auto history = engine.version_store().HistoryForTable("aud", "t");
    if (history.empty()) return 1;
    const std::string ts = history.front().timestamp;
    if (!engine.ExecuteSql(session, "REVERT t AT_OR_BEFORE \"" + ts + "\";").ok()) {
        return 1;
    }

    const std::filesystem::path root_path(root);
    if (!std::filesystem::exists(root_path / "runtime_state.tsv")) return 1;
    if (!std::filesystem::exists(root_path / "version_history.tsv")) return 1;

    // Audit condition: no extra snapshot dump files are created for point-in-time
    // restore; only state + history are used.
    for (const auto &entry : std::filesystem::directory_iterator(root_path)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        const auto filename = entry.path().filename().string();
        if (filename == "runtime_state.tsv" || filename == "version_history.tsv") {
            continue;
        }
        // Temporary files may appear only transiently during save; after operation
        // they must not remain.
        if (entry.path().extension() == ".tmp") {
            return 1;
        }
        // Any other persistent file would indicate additional snapshot artifacts.
        return 1;
    }

    std::filesystem::remove_all(root);
    return 0;
}
