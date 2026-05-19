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

} // namespace

int main() {
    const std::string root = "./test_data_default_acceptance";
    std::filesystem::remove_all(root);

    dbms::core::DbmsEngine engine(root);
    dbms::core::SessionContext session;
    session.client_id = "default_acceptance";

    if (!ExpectOk(engine.ExecuteSql(session, "CREATE DATABASE ddef;"))) return 1;
    if (!ExpectOk(engine.ExecuteSql(session, "USE ddef;"))) return 1;

    if (!ExpectOk(engine.ExecuteSql(
            session,
            "CREATE TABLE cfg (id INT INDEXED, name STRING DEFAULT \"guest\", score INT DEFAULT 0, note STRING);"))) {
        return 1;
    }

    // DEFAULT is applied when column is omitted.
    if (!ExpectOk(engine.ExecuteSql(
            session, "INSERT INTO cfg (id, note) VALUE (1, \"n1\");"))) {
        return 1;
    }
    auto row_1 = engine.ExecuteSql(
        session, "SELECT name, score, note FROM cfg WHERE id == 1;");
    if (!row_1.ok() || row_1.value->rows.size() != 1) return 1;
    if (!std::holds_alternative<std::string>(row_1.value->rows[0].values[0]) ||
        std::get<std::string>(row_1.value->rows[0].values[0]) != "guest") {
        return 1;
    }
    if (!std::holds_alternative<std::int64_t>(row_1.value->rows[0].values[1]) ||
        std::get<std::int64_t>(row_1.value->rows[0].values[1]) != 0) {
        return 1;
    }
    if (!std::holds_alternative<std::string>(row_1.value->rows[0].values[2]) ||
        std::get<std::string>(row_1.value->rows[0].values[2]) != "n1") {
        return 1;
    }

    // Current engine semantics: NULL in a DEFAULT column is normalized to DEFAULT.
    if (!ExpectOk(engine.ExecuteSql(
            session, "INSERT INTO cfg (id, name, score, note) VALUE (2, NULL, NULL, \"n2\");"))) {
        return 1;
    }
    auto row_2 = engine.ExecuteSql(
        session, "SELECT name, score FROM cfg WHERE id == 2;");
    if (!row_2.ok() || row_2.value->rows.size() != 1) return 1;
    if (!std::holds_alternative<std::string>(row_2.value->rows[0].values[0]) ||
        std::get<std::string>(row_2.value->rows[0].values[0]) != "guest") {
        return 1;
    }
    if (!std::holds_alternative<std::int64_t>(row_2.value->rows[0].values[1]) ||
        std::get<std::int64_t>(row_2.value->rows[0].values[1]) != 0) {
        return 1;
    }

    // Type mismatch against DEFAULT-type column must fail on insert.
    if (!ExpectError(engine.ExecuteSql(
            session, "INSERT INTO cfg (id, score, note) VALUE (3, \"bad\", \"n3\");"),
            dbms::common::ErrorCode::kConstraintViolation)) {
        return 1;
    }

    // NOT_NULL + DEFAULT NULL is rejected at parse/validation time.
    if (!ExpectError(engine.ExecuteSql(
            session, "CREATE TABLE bad_default (id INT NOT_NULL DEFAULT NULL);"),
            dbms::common::ErrorCode::kParseError)) {
        return 1;
    }

    std::filesystem::remove_all(root);
    return 0;
}
