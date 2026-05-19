#include <iostream>
#include <string>

#include "core/dbms_engine.hpp"
#include "common/types.hpp"

namespace {

    std::string EscapeJson(const std::string &value) {
        std::string escaped;
        escaped.reserve(value.size());
        for (char ch : value) {
            if (ch == '"' || ch == '\\') {
                escaped.push_back('\\');
            }
            escaped.push_back(ch);
        }
        return escaped;
    }

    std::string ValueToJson(const dbms::common::Value &value) {
        if (std::holds_alternative<std::monostate>(value)) {
            return "null";
        }
        if (std::holds_alternative<std::int64_t>(value)) {
            return std::to_string(std::get<std::int64_t>(value));
        }
        return "\"" + EscapeJson(std::get<std::string>(value)) + "\"";
    }

    void PrintSelectJson(const dbms::execution::QueryResult &result) {
        std::cout << "[";
        for (std::size_t row_index = 0; row_index < result.rows.size();
             ++row_index) {
            if (row_index != 0) {
                std::cout << ", ";
            }
            std::cout << "{";
            const auto &row = result.rows[row_index];
            for (std::size_t column_index = 0;
                 column_index < result.column_names.size() &&
                 column_index < row.values.size();
                 ++column_index) {
                if (column_index != 0) {
                    std::cout << ", ";
                }
                std::cout << "\"" << EscapeJson(result.column_names[column_index])
                          << "\": " << ValueToJson(row.values[column_index]);
            }
            std::cout << "}";
        }
        std::cout << "]\n";
    }

} // namespace

// this file boots the cli client and routes commands through the dbms engine.
int main() {
    dbms::core::DbmsEngine engine("./data");
    dbms::core::SessionContext session;
    session.client_id = "cli";

    std::string sql;
    std::getline(std::cin, sql);

    auto result = engine.ExecuteSql(session, sql);
    if (!result.ok()) {
        std::cout << "error: " << result.error->message << "\n";
        return 1;
    }

    if (!result.value->column_names.empty()) {
        PrintSelectJson(*result.value);
        return 0;
    }

    std::cout << result.value->message << "\n";
    return 0;
}
