#include <cassert>
#include <iostream>

#include "common/error.hpp"
#include "common/types.hpp"
#include "core/dbms_engine.hpp"
#include "parser/lexer.hpp"

// this file provides a smoke test for the end-to-end dbms execution pipeline.
int main() {
    // comparison test
    using namespace dbms::common;

    Value int_small = std::int64_t{2};
    Value int_large = std::int64_t{10};
    Value str_a = std::string{"abc"};
    Value str_b = std::string{"abd"};
    Value null_value = std::monostate{};

    assert(GetValueType(int_small) == ValueType::kInt64);
    assert(GetValueType(str_a) == ValueType::kString);
    assert(GetValueType(null_value) == ValueType::kNull);

    assert(CompareValues(int_small, int_large) < 0);
    assert(CompareValues(int_large, int_small) > 0);
    assert(CompareValues(int_large, std::int64_t{10}) == 0);
    assert(CompareValues(str_a, str_b) < 0);
    assert(CompareValues(null_value, std::monostate{}) == 0);

    assert(CanAssignToType(int_small, ValueType::kInt64, false));
    assert(!CanAssignToType(str_a, ValueType::kInt64, false));
    assert(CanAssignToType(null_value, ValueType::kString, true));
    assert(!CanAssignToType(null_value, ValueType::kString, false));

    // dbms engine test
    dbms::core::DbmsEngine engine("./test_data");
    dbms::core::SessionContext session;
    session.client_id = "smoke";

    auto result = engine.ExecuteSql(session, "DROP DATABASE test_db;");
    if (!result.ok() &&
        result.error->code != dbms::common::ErrorCode::kNotFound) {
        std::cout << "pipeline_error\n";
        return 1;
    }
    result = engine.ExecuteSql(session, "CREATE DATABASE test_db;");
    if (!result.ok()) {
        std::cout << "pipeline_error\n";
        return 1;
    }
    result = engine.ExecuteSql(session, "USE test_db;");
    if (!result.ok()) {
        std::cout << "pipeline_error\n";
        return 1;
    }
    result = engine.ExecuteSql(
        session, "CREATE TABLE test (id INT INDEXED, score INT, name STRING);");
    if (!result.ok()) {
        std::cout << "pipeline_error\n";
        return 1;
    }
    result = engine.ExecuteSql(
        session, "INSERT INTO test (id, score, name) VALUES (1, 10, \"Alice\");");
    if (!result.ok()) {
        std::cout << "pipeline_error\n";
        return 1;
    }
    result = engine.ExecuteSql(session, "SELECT * FROM test;");
    if (!result.ok() || result.value->rows.size() != 1) {
        std::cout << "pipeline_error\n";
        return 1;
    }
    result = engine.ExecuteSql(session, "UPDATE test SET name = \"Bob\" WHERE id == 1;");
    if (!result.ok()) {
        std::cout << "pipeline_error\n";
        return 1;
    }
    result = engine.ExecuteSql(session, "SELECT name FROM test WHERE id == 1;");
    if (!result.ok() || result.value->rows.size() != 1 ||
        !std::holds_alternative<std::string>(result.value->rows[0].values[0]) ||
        std::get<std::string>(result.value->rows[0].values[0]) != "Bob") {
        std::cout << "pipeline_error\n";
        return 1;
    }
    result = engine.ExecuteSql(session, "DELETE FROM test WHERE id == 1;");
    if (!result.ok()) {
        std::cout << "pipeline_error\n";
        return 1;
    }
    result = engine.ExecuteSql(session, "SELECT * FROM test;");
    if (!result.ok() || !result.value->rows.empty()) {
        std::cout << "pipeline_error\n";
        return 1;
    }

    result = engine.ExecuteSql(
        session,
        "INSERT INTO test (id, score, name) VALUES (2, 20, \"A\"), (3, 30, \"B\");");
    if (!result.ok()) {
        std::cout << "pipeline_error\n";
        return 1;
    }
    result = engine.ExecuteSql(session, "UPDATE test SET score = id WHERE id == 2;");
    if (!result.ok()) {
        std::cout << "pipeline_error\n";
        return 1;
    }
    result = engine.ExecuteSql(session, "SELECT score FROM test WHERE id == 2;");
    if (!result.ok() || result.value->rows.size() != 1 ||
        !std::holds_alternative<std::int64_t>(result.value->rows[0].values[0]) ||
        std::get<std::int64_t>(result.value->rows[0].values[0]) != 2) {
        std::cout << "pipeline_error\n";
        return 1;
    }

    result = engine.ExecuteSql(session, "SELECT COUNT(*) AS c FROM test;");
    if (!result.ok() || result.value->rows.size() != 1 ||
        !std::holds_alternative<std::int64_t>(result.value->rows[0].values[0]) ||
        std::get<std::int64_t>(result.value->rows[0].values[0]) != 2) {
        std::cout << "pipeline_error\n";
        return 1;
    }
    result = engine.ExecuteSql(session, "SELECT SUM(id), AVG(id) FROM test;");
    if (!result.ok() || result.value->rows.size() != 1 ||
        result.value->rows[0].values.size() != 2 ||
        !std::holds_alternative<std::int64_t>(result.value->rows[0].values[0]) ||
        !std::holds_alternative<std::int64_t>(result.value->rows[0].values[1]) ||
        std::get<std::int64_t>(result.value->rows[0].values[0]) != 5 ||
        std::get<std::int64_t>(result.value->rows[0].values[1]) != 2) {
        std::cout << "pipeline_error\n";
        return 1;
    }

    // tokenize test
    const std::string simple_query = "SELECT * FROM test WHERE AGE > 15 AND SEX = M;";
    const auto simple_tokens = dbms::parser::Lexer::Tokenize(simple_query);

    assert(simple_tokens.size() == 13);
    assert(simple_tokens[0].lexeme == "SELECT");
    assert(simple_tokens[1].lexeme == "*");
    assert(simple_tokens[2].lexeme == "FROM");
    assert(simple_tokens[3].lexeme == "test");
    assert(simple_tokens[4].lexeme == "WHERE");
    assert(simple_tokens[5].lexeme == "AGE");
    assert(simple_tokens[6].lexeme == ">");
    assert(simple_tokens[7].lexeme == "15");
    assert(simple_tokens[8].lexeme == "AND");
    assert(simple_tokens[9].lexeme == "SEX");
    assert(simple_tokens[10].lexeme == "=");
    assert(simple_tokens[11].lexeme == "M");
    assert(simple_tokens[12].lexeme == ";");

    const std::string complex_query =
        "SELECT name, age FROM db1.users WHERE age >= 18 AND city != \"Moscow\" OR (score <= 10);";
    const auto complex_tokens = dbms::parser::Lexer::Tokenize(complex_query);

    assert(complex_tokens.size() == 23);
    assert(complex_tokens[0].lexeme == "SELECT");
    assert(complex_tokens[1].lexeme == "name");
    assert(complex_tokens[2].lexeme == ",");
    assert(complex_tokens[3].lexeme == "age");
    assert(complex_tokens[4].lexeme == "FROM");
    assert(complex_tokens[5].lexeme == "db1");
    assert(complex_tokens[6].lexeme == ".");
    assert(complex_tokens[7].lexeme == "users");
    assert(complex_tokens[8].lexeme == "WHERE");
    assert(complex_tokens[9].lexeme == "age");
    assert(complex_tokens[10].lexeme == ">=");
    assert(complex_tokens[11].lexeme == "18");
    assert(complex_tokens[12].lexeme == "AND");
    assert(complex_tokens[13].lexeme == "city");
    assert(complex_tokens[14].lexeme == "!=");
    assert(complex_tokens[15].lexeme == "\"Moscow\"");
    assert(complex_tokens[16].lexeme == "OR");
    assert(complex_tokens[17].lexeme == "(");
    assert(complex_tokens[18].lexeme == "score");
    assert(complex_tokens[19].lexeme == "<=");
    assert(complex_tokens[20].lexeme == "10");
    assert(complex_tokens[21].lexeme == ")");
    assert(complex_tokens[22].lexeme == ";");

    std::cout << "pipeline_ok\n";

    return 0;
}
