#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "common/types.hpp"
#include "core/dbms_engine.hpp"

namespace {

    const dbms::common::InternedString &
    RequireInternedString(const dbms::common::Value &value) {
        if (!std::holds_alternative<dbms::common::InternedString>(value)) {
            throw std::runtime_error("row value is not interned");
        }
        return std::get<dbms::common::InternedString>(value);
    }

} // namespace

int main() {
    const std::string root = "./test_data_string_interning";
    std::filesystem::remove_all(root);

    {
        dbms::core::DbmsEngine engine(root);
        dbms::core::SessionContext session;
        session.client_id = "intern";

        if (!engine.ExecuteSql(session, "CREATE DATABASE sdb;").ok()) return 1;
        if (!engine.ExecuteSql(session, "USE sdb;").ok()) return 1;
        if (!engine.ExecuteSql(
                session,
                "CREATE TABLE t (id INT INDEXED, name STRING DEFAULT \"anon\", city STRING, code STRING INDEXED);")
                .ok()) {
            return 1;
        }

        if (!engine.ExecuteSql(
                session,
                "INSERT INTO t (id, name, city, code) VALUES "
                "(1, \"Alex\", \"Moscow\", \"code-1\"), "
                "(2, \"Alex\", \"Moscow\", \"code-2\"), "
                "(3, \"Alex\", \"Moscow\", \"code-3\");")
                .ok()) {
            return 1;
        }

        if (!engine.ExecuteSql(
                session,
                "INSERT INTO t (id, city, code) VALUES (4, \"Moscow\", \"code-4\");")
                .ok()) {
            return 1;
        }

        auto &table = engine.runtime_state().databases.at("sdb").tables.at("t");
        const auto rows = table.heap->ScanAll();
        if (rows.size() != 4) return 1;

        const auto &name_ref_1 = RequireInternedString(rows[0].values[1]);
        const auto &name_ref_2 = RequireInternedString(rows[1].values[1]);
        const auto &name_ref_3 = RequireInternedString(rows[2].values[1]);
        const auto &name_ref_4 = RequireInternedString(rows[3].values[1]);
        const auto &city_ref_1 = RequireInternedString(rows[0].values[2]);
        const auto &city_ref_2 = RequireInternedString(rows[1].values[2]);
        if (name_ref_1.get() != name_ref_2.get() ||
            name_ref_1.get() != name_ref_3.get()) {
            return 1;
        }
        if (city_ref_1.get() != city_ref_2.get()) {
            return 1;
        }
        if (dbms::common::AsString(rows[3].values[1]) != "anon") {
            return 1;
        }
        if (engine.string_pool().UniqueCount() != 7) {
            return 1;
        }

        if (!engine.ExecuteSql(
                session,
                "UPDATE t SET name = \"Alex\", city = \"Moscow\", code = \"code-2\" WHERE id == 2;")
                .ok()) {
            return 1;
        }
        if (engine.string_pool().UniqueCount() != 7) {
            return 1;
        }

        if (!engine.ExecuteSql(
                session, "UPDATE t SET city = \"Kazan\" WHERE id == 3;")
                .ok()) {
            return 1;
        }
        if (engine.string_pool().UniqueCount() != 8) {
            return 1;
        }

        auto filtered =
            engine.ExecuteSql(session, "SELECT city FROM t WHERE code == \"code-2\";");
        if (!filtered.ok() || filtered.value->rows.size() != 1) {
            return 1;
        }
    }

    {
        dbms::core::DbmsEngine engine(root);
        dbms::core::SessionContext session;
        session.client_id = "intern_reload";

        if (!engine.ExecuteSql(session, "USE sdb;").ok()) return 1;
        auto &table = engine.runtime_state().databases.at("sdb").tables.at("t");
        const auto rows = table.heap->ScanAll();
        if (rows.size() != 4) return 1;

        const auto &name_ref_1 = RequireInternedString(rows[0].values[1]);
        const auto &name_ref_2 = RequireInternedString(rows[1].values[1]);
        const auto &code_ref_2 = RequireInternedString(rows[1].values[3]);
        if (name_ref_1.get() != name_ref_2.get()) {
            return 1;
        }
        if (dbms::common::AsString(code_ref_2) != "code-2") {
            return 1;
        }

        if (engine.string_pool().UniqueCount() != 8) {
            return 1;
        }

        auto indexed =
            engine.ExecuteSql(session, "SELECT name FROM t WHERE code == \"code-2\";");
        if (!indexed.ok() || indexed.value->rows.size() != 1) {
            return 1;
        }

        auto like =
            engine.ExecuteSql(session, "SELECT id FROM t WHERE city LIKE \"Mos.*\";");
        if (!like.ok() || like.value->rows.size() != 3) {
            return 1;
        }
    }

    std::filesystem::remove_all(root);
    return 0;
}
