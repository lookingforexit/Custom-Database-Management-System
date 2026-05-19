#include <filesystem>

#include "core/dbms_engine.hpp"

int main() {
    const std::string root = "./test_data_string_interning";
    std::filesystem::remove_all(root);

    dbms::core::DbmsEngine engine(root);
    dbms::core::SessionContext session;
    session.client_id = "intern";

    if (!engine.ExecuteSql(session, "CREATE DATABASE sdb;").ok()) return 1;
    if (!engine.ExecuteSql(session, "USE sdb;").ok()) return 1;
    if (!engine.ExecuteSql(
            session, "CREATE TABLE t (id INT INDEXED, name STRING, city STRING);")
            .ok()) {
        return 1;
    }

    // repeated strings should be interned once per distinct value
    if (!engine.ExecuteSql(
            session,
            "INSERT INTO t (id, name, city) VALUES (1, \"Alex\", \"Moscow\"), (2, \"Alex\", \"Moscow\"), (3, \"Alex\", \"Moscow\");")
            .ok()) {
        return 1;
    }

    const auto count_after_insert = engine.string_pool().UniqueCount();
    if (count_after_insert != 2) {
        return 1;
    }

    if (!engine.ExecuteSql(
            session, "UPDATE t SET name = \"Alex\", city = \"Moscow\" WHERE id == 2;")
            .ok()) {
        return 1;
    }
    if (engine.string_pool().UniqueCount() != 2) {
        return 1;
    }

    if (!engine.ExecuteSql(
            session, "UPDATE t SET city = \"Kazan\" WHERE id == 3;")
            .ok()) {
        return 1;
    }
    if (engine.string_pool().UniqueCount() != 3) {
        return 1;
    }

    std::filesystem::remove_all(root);
    return 0;
}
