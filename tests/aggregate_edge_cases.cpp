#include "common/error.hpp"
#include "core/dbms_engine.hpp"

#include <filesystem>
#include <string>

namespace {

    bool ExpectOk(const dbms::common::Result<dbms::execution::QueryResult> &result) {
        return result.ok();
    }

    bool ExpectError(const dbms::common::Result<dbms::execution::QueryResult> &result,
                     dbms::common::ErrorCode code) {
        return !result.ok() && result.error.has_value() && result.error->code == code;
    }

    bool ExpectIntCell(const dbms::execution::QueryResult &result, std::size_t column,
                       std::int64_t expected) {
        if (result.rows.size() != 1 || result.rows[0].values.size() <= column) {
            return false;
        }
        const auto &value = result.rows[0].values[column];
        return std::holds_alternative<std::int64_t>(value) &&
               std::get<std::int64_t>(value) == expected;
    }

} // namespace

int main() {
    const std::string root = "./test_data_aggregate_edges";
    std::filesystem::remove_all(root);

    dbms::core::DbmsEngine engine(root);
    dbms::core::SessionContext session;
    session.client_id = "aggregate_edges";

    if (!ExpectOk(engine.ExecuteSql(session, "CREATE DATABASE adb;"))) return 1;
    if (!ExpectOk(engine.ExecuteSql(session, "USE adb;"))) return 1;
    if (!ExpectOk(engine.ExecuteSql(
            session, "CREATE TABLE agg (id INT INDEXED, v INT, s STRING);"))) {
        return 1;
    }

    // Empty table aggregates.
    auto empty_agg = engine.ExecuteSql(
        session, "SELECT COUNT(*), COUNT(v), SUM(v), AVG(v) FROM agg;");
    if (!empty_agg.ok()) return 1;
    if (!ExpectIntCell(*empty_agg.value, 0, 0)) return 1;
    if (!ExpectIntCell(*empty_agg.value, 1, 0)) return 1;
    if (!ExpectIntCell(*empty_agg.value, 2, 0)) return 1;
    if (!std::holds_alternative<std::monostate>(empty_agg.value->rows[0].values[3])) {
        return 1;
    }

    // Mix of ints, nulls and negative values.
    if (!ExpectOk(engine.ExecuteSql(
            session,
            "INSERT INTO agg (id, v, s) VALUES "
            "(1, 10, \"a\"),"
            "(2, NULL, \"b\"),"
            "(3, -5, \"c\"),"
            "(4, 0, \"d\");"))) {
        return 1;
    }

    auto mixed_agg = engine.ExecuteSql(
        session, "SELECT COUNT(*), COUNT(v), SUM(v), AVG(v) FROM agg;");
    if (!mixed_agg.ok()) return 1;
    if (!ExpectIntCell(*mixed_agg.value, 0, 4)) return 1;
    if (!ExpectIntCell(*mixed_agg.value, 1, 3)) return 1;
    if (!ExpectIntCell(*mixed_agg.value, 2, 5)) return 1;
    if (!ExpectIntCell(*mixed_agg.value, 3, 1)) return 1;

    // WHERE-filtered no rows: AVG -> NULL, COUNT/SUM -> 0.
    auto no_rows_agg = engine.ExecuteSql(
        session,
        "SELECT COUNT(*), COUNT(v), SUM(v), AVG(v) FROM agg WHERE id == 999;");
    if (!no_rows_agg.ok()) return 1;
    if (!ExpectIntCell(*no_rows_agg.value, 0, 0)) return 1;
    if (!ExpectIntCell(*no_rows_agg.value, 1, 0)) return 1;
    if (!ExpectIntCell(*no_rows_agg.value, 2, 0)) return 1;
    if (!std::holds_alternative<std::monostate>(no_rows_agg.value->rows[0].values[3])) {
        return 1;
    }

    // SUM/AVG on STRING should fail with type mismatch.
    if (!ExpectError(engine.ExecuteSql(session, "SELECT SUM(s) FROM agg;"),
                     dbms::common::ErrorCode::kTypeMismatch)) {
        return 1;
    }
    if (!ExpectError(engine.ExecuteSql(session, "SELECT AVG(s) FROM agg;"),
                     dbms::common::ErrorCode::kTypeMismatch)) {
        return 1;
    }

    std::filesystem::remove_all(root);
    return 0;
}
