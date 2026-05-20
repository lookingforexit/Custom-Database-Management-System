#include <filesystem>
#include <string>

#include "common/error.hpp"
#include "core/dbms_engine.hpp"

namespace {
    bool ExpectOk(const dbms::common::Result<dbms::execution::QueryResult> &result) {
        return result.ok();
    }
}

int main() {
    const std::string root = "./test_data_index_service";
    std::filesystem::remove_all(root);

    dbms::core::DbmsEngine engine(root);
    dbms::core::SessionContext session;
    session.client_id = "index_service";

    if (!ExpectOk(engine.ExecuteSql(session, "CREATE DATABASE idb;"))) return 1;
    if (!ExpectOk(engine.ExecuteSql(session, "USE idb;"))) return 1;
    if (!ExpectOk(engine.ExecuteSql(
            session, "CREATE TABLE t (id INT INDEXED, name STRING, score INT);"))) {
        return 1;
    }
    if (!ExpectOk(engine.ExecuteSql(
            session,
            "INSERT INTO t (id, name, score) VALUES (1, \"A\", 10), (2, \"B\", 20), (3, \"C\", 30);"))) {
        return 1;
    }

    auto check_before = engine.ExecuteSql(session, "CHECK INDEX;");
    if (!check_before.ok()) return 1;

    if (!ExpectOk(engine.ExecuteSql(session, "REBUILD INDEX;"))) return 1;

    auto check_after = engine.ExecuteSql(session, "CHECK INDEX;");
    if (!check_after.ok()) return 1;

    // Service command is forbidden in active tx.
    if (!ExpectOk(engine.ExecuteSql(session, "BEGIN;"))) return 1;
    auto invalid_in_tx = engine.ExecuteSql(session, "CHECK INDEX;");
    if (invalid_in_tx.ok() || !invalid_in_tx.error.has_value() ||
        invalid_in_tx.error->code != dbms::common::ErrorCode::kValidationError) {
        return 1;
    }
    if (!ExpectOk(engine.ExecuteSql(session, "ROLLBACK;"))) return 1;

    // Data path still correct after rebuild.
    auto select = engine.ExecuteSql(session, "SELECT score FROM t WHERE id == 2;");
    if (!select.ok() || select.value->rows.size() != 1 ||
        !std::holds_alternative<std::int64_t>(select.value->rows[0].values[0]) ||
        std::get<std::int64_t>(select.value->rows[0].values[0]) != 20) {
        return 1;
    }

    std::filesystem::remove_all(root);
    return 0;
}
