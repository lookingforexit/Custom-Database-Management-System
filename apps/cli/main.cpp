#include <iostream>
#include <fstream>
#include <string>
#include <vector>

#include "core/dbms_engine.hpp"
#include "common/types.hpp"

namespace {

    const char *ErrorCodeName(dbms::common::ErrorCode code) {
        switch (code) {
            case dbms::common::ErrorCode::kOk:
                return "OK";
            case dbms::common::ErrorCode::kParseError:
                return "PARSE_ERROR";
            case dbms::common::ErrorCode::kSemanticError:
                return "SEMANTIC_ERROR";
            case dbms::common::ErrorCode::kValidationError:
                return "VALIDATION_ERROR";
            case dbms::common::ErrorCode::kStorageError:
                return "STORAGE_ERROR";
            case dbms::common::ErrorCode::kAuthorizationError:
                return "AUTHORIZATION_ERROR";
            case dbms::common::ErrorCode::kNetworkError:
                return "NETWORK_ERROR";
            case dbms::common::ErrorCode::kNotFound:
                return "NOT_FOUND";
            case dbms::common::ErrorCode::kAlreadyExists:
                return "ALREADY_EXISTS";
            case dbms::common::ErrorCode::kConstraintViolation:
                return "CONSTRAINT_VIOLATION";
            case dbms::common::ErrorCode::kTypeMismatch:
                return "TYPE_MISMATCH";
            case dbms::common::ErrorCode::kNotImplemented:
                return "NOT_IMPLEMENTED";
        }
        return "UNKNOWN";
    }

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

    std::vector<std::string> SplitStatements(const std::string &input) {
        std::vector<std::string> statements;
        std::string current;
        bool in_string = false;
        bool escaped = false;

        for (char ch : input) {
            current.push_back(ch);
            if (in_string) {
                if (escaped) {
                    escaped = false;
                    continue;
                }
                if (ch == '\\') {
                    escaped = true;
                    continue;
                }
                if (ch == '"') {
                    in_string = false;
                }
                continue;
            }

            if (ch == '"') {
                in_string = true;
                continue;
            }
            if (ch == ';') {
                statements.push_back(current);
                current.clear();
            }
        }

        if (!current.empty()) {
            statements.push_back(current);
        }
        return statements;
    }

    int ExecuteAndPrint(dbms::core::DbmsEngine &engine,
                        dbms::core::SessionContext &session,
                        const std::string &sql,
                        std::size_t statement_index) {
        auto result = engine.ExecuteSql(session, sql);
        if (!result.ok()) {
            std::cout << "error[" << statement_index << "]: type="
                      << ErrorCodeName(result.error->code) << " code="
                      << static_cast<int>(result.error->code)
                      << " message=" << result.error->message
                      << " sql=\"" << EscapeJson(sql) << "\"\n";
            return 1;
        }
        if (!result.value->column_names.empty()) {
            PrintSelectJson(*result.value);
        } else {
            std::cout << result.value->message << "\n";
        }
        return 0;
    }

} // namespace

// this file boots the cli client and routes commands through the dbms engine.
int main(int argc, char **argv) {
    dbms::core::DbmsEngine engine("./data");
    dbms::core::SessionContext session;
    session.client_id = "cli";

    // batch mode: ./dbms_cli script.sql
    if (argc == 2) {
        std::ifstream input_file(argv[1]);
        if (!input_file.is_open()) {
            std::cout << "error: cannot open script file: " << argv[1] << "\n";
            return 1;
        }
        std::string script((std::istreambuf_iterator<char>(input_file)),
                           std::istreambuf_iterator<char>());
        const auto statements = SplitStatements(script);
        int exit_code = 0;
        std::size_t statement_index = 1;
        for (const auto &statement : statements) {
            const bool has_non_space =
                statement.find_first_not_of(" \t\r\n") != std::string::npos;
            if (!has_non_space) {
                continue;
            }
            if (ExecuteAndPrint(engine, session, statement, statement_index) != 0) {
                exit_code = 1;
            }
            ++statement_index;
        }
        return exit_code;
    }

    // interactive mode: read multiline statements until ';'
    std::string buffer;
    std::string line;
    std::size_t statement_index = 1;
    while (std::getline(std::cin, line)) {
        buffer += line;
        buffer.push_back('\n');
        const auto statements = SplitStatements(buffer);
        if (statements.empty()) {
            continue;
        }
        const bool ended_with_semicolon =
            !buffer.empty() && buffer.find(';') != std::string::npos &&
            buffer.find_last_not_of(" \t\r\n") != std::string::npos &&
            buffer.find_last_of(';') >= buffer.find_first_not_of(" \t\r\n");

        std::size_t executable_count = statements.size();
        if (!buffer.empty() && buffer.back() != ';') {
            executable_count = statements.size() - 1;
        }

        for (std::size_t i = 0; i < executable_count; ++i) {
            const auto &statement = statements[i];
            const bool has_non_space =
                statement.find_first_not_of(" \t\r\n") != std::string::npos;
            if (!has_non_space) {
                continue;
            }
            ExecuteAndPrint(engine, session, statement, statement_index);
            ++statement_index;
        }

        if (executable_count < statements.size()) {
            buffer = statements.back();
        } else {
            buffer.clear();
        }
        (void)ended_with_semicolon;
    }

    return 0;
}
