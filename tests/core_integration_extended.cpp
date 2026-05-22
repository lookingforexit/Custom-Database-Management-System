#include <filesystem>
#include <string>

#include "common/error.hpp"
#include "common/types.hpp"
#include "core/dbms_engine.hpp"

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
    const std::string root = "./test_data_core_extended";
    std::filesystem::remove_all(root);

    dbms::core::DbmsEngine engine(root);
    dbms::core::SessionContext session;
    session.client_id = "core_extended";

    if (!ExpectOk(engine.ExecuteSql(session, "CREATE DATABASE extdb;"))) return 1;
    if (!ExpectOk(engine.ExecuteSql(session, "USE extdb;"))) return 1;
    if (!ExpectOk(engine.ExecuteSql(
            session,
            "CREATE TABLE users (id INT INDEXED, name STRING NOT_NULL, city STRING DEFAULT \"unknown\", score INT);"))) {
        return 1;
    }

    // Constraint/validation checks.
    if (!ExpectError(engine.ExecuteSql(
            session, "INSERT INTO users (id, name) VALUE (1, NULL);"),
            dbms::common::ErrorCode::kConstraintViolation)) {
        return 1;
    }
    if (!ExpectError(engine.ExecuteSql(
            session, "INSERT INTO users (id, name) VALUE (NULL, \"A\");"),
            dbms::common::ErrorCode::kConstraintViolation)) {
        return 1;
    }
    if (!ExpectOk(engine.ExecuteSql(
            session, "INSERT INTO users (id, name, score) VALUES (1, \"Ann\", 10), (2, \"Bob\", 20), (3, \"Cara\", 30);"))) {
        return 1;
    }
    if (!ExpectError(engine.ExecuteSql(
            session, "INSERT INTO users (id, name, score) VALUE (1, \"Dup\", 99);"),
            dbms::common::ErrorCode::kConstraintViolation)) {
        return 1;
    }

    // DEFAULT propagation.
    auto default_select = engine.ExecuteSql(
        session, "SELECT city FROM users WHERE id == 1;");
    if (!default_select.ok() || default_select.value->rows.size() != 1 ||
        dbms::common::GetValueType(default_select.value->rows[0].values[0]) !=
            dbms::common::ValueType::kString ||
        dbms::common::AsString(default_select.value->rows[0].values[0]) != "unknown") {
        return 1;
    }

    // WHERE: BETWEEN is [l, r), LIKE, AND/OR with parenthesis.
    auto between_select = engine.ExecuteSql(
        session, "SELECT id FROM users WHERE id BETWEEN 1 AND 3;");
    if (!between_select.ok() || between_select.value->rows.size() != 2) return 1;

    auto like_select = engine.ExecuteSql(
        session, "SELECT id FROM users WHERE name LIKE \"A.*\";");
    if (!like_select.ok() || like_select.value->rows.size() != 1) return 1;

    auto logical_select = engine.ExecuteSql(
        session,
        "SELECT id FROM users WHERE (id == 1 OR id == 3) AND score >= 10;");
    if (!logical_select.ok() || logical_select.value->rows.size() != 2) return 1;

    // Aggregates.
    auto aggregate_select = engine.ExecuteSql(
        session, "SELECT COUNT(*), SUM(score), AVG(score) FROM users;");
    if (!aggregate_select.ok() || aggregate_select.value->rows.size() != 1 ||
        aggregate_select.value->rows[0].values.size() != 3 ||
        !std::holds_alternative<std::int64_t>(aggregate_select.value->rows[0].values[0]) ||
        !std::holds_alternative<std::int64_t>(aggregate_select.value->rows[0].values[1]) ||
        !std::holds_alternative<std::int64_t>(aggregate_select.value->rows[0].values[2]) ||
        std::get<std::int64_t>(aggregate_select.value->rows[0].values[0]) != 3 ||
        std::get<std::int64_t>(aggregate_select.value->rows[0].values[1]) != 60 ||
        std::get<std::int64_t>(aggregate_select.value->rows[0].values[2]) != 20) {
        return 1;
    }

    // Type mismatch must return error and not corrupt state.
    if (!ExpectError(engine.ExecuteSql(
            session, "UPDATE users SET score = \"bad\" WHERE id == 1;"),
            dbms::common::ErrorCode::kConstraintViolation)) {
        return 1;
    }
    auto stable_after_error = engine.ExecuteSql(
        session, "SELECT score FROM users WHERE id == 1;");
    if (!stable_after_error.ok() || stable_after_error.value->rows.size() != 1 ||
        !std::holds_alternative<std::int64_t>(
            stable_after_error.value->rows[0].values[0]) ||
        std::get<std::int64_t>(stable_after_error.value->rows[0].values[0]) != 10) {
        return 1;
    }

    // Parser rejection for mixed-case keyword must not break subsequent execution.
    if (!ExpectError(engine.ExecuteSql(session, "SeLeCt * FROM users;"),
                     dbms::common::ErrorCode::kParseError)) {
        return 1;
    }
    auto final_select = engine.ExecuteSql(session, "SELECT * FROM users;");
    if (!final_select.ok() || final_select.value->rows.size() != 3) return 1;

    std::filesystem::remove_all(root);
    return 0;
}
