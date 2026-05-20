#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <optional>
#include <sstream>

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include "core/dbms_engine.hpp"
#include "common/error_contract.hpp"
#include "common/types.hpp"
#include "network/protocol.hpp"

namespace {
    void PrintUsage(const char *program_name) {
        std::cout << "Usage:\n";
        std::cout << "  " << program_name << " [script.sql]\n";
        std::cout << "  " << program_name
                  << " --server host:port [--jwt token] [script.sql]\n";
        std::cout << "  " << program_name << " --demo [--server host:port] [--jwt token]\n";
        std::cout << "  " << program_name << " --help\n";
    }

    std::vector<std::string> BuildDemoStatements() {
        return {
            "CREATE DATABASE demo;",
            "USE demo;",
            "CREATE TABLE users (id INT INDEXED, name STRING, score INT DEFAULT 0);",
            "INSERT INTO users (id, name) VALUES (1, \"Ann\"), (2, \"Bob\");",
            "UPDATE users SET score = 42 WHERE id == 2;",
            "SELECT * FROM users;",
            "SELECT COUNT(*), SUM(score), AVG(score) FROM users;",
        };
    }

    std::string ValueToJson(const dbms::common::Value &value) {
        if (std::holds_alternative<std::monostate>(value)) {
            return "null";
        }
        if (std::holds_alternative<std::int64_t>(value)) {
            return std::to_string(std::get<std::int64_t>(value));
        }
        return "\"" + dbms::common::EscapeJsonText(std::get<std::string>(value)) +
               "\"";
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
                std::cout << "\""
                          << dbms::common::EscapeJsonText(
                                 result.column_names[column_index])
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

    std::optional<std::pair<std::string, int>>
    ParseEndpoint(const std::string &endpoint) {
        const auto colon_position = endpoint.rfind(':');
        if (colon_position == std::string::npos || colon_position == 0 ||
            colon_position + 1 >= endpoint.size()) {
            return std::nullopt;
        }
        std::string host = endpoint.substr(0, colon_position);
        int port = 0;
        try {
            port = std::stoi(endpoint.substr(colon_position + 1));
        } catch (...) {
            return std::nullopt;
        }
        if (port <= 0 || port > 65535) {
            return std::nullopt;
        }
        return std::make_pair(host, port);
    }

    bool ReadLine(int socket_fd, std::string &line) {
        line.clear();
        char ch = '\0';
        while (true) {
            const auto bytes = recv(socket_fd, &ch, 1, 0);
            if (bytes <= 0) {
                return false;
            }
            if (ch == '\n') {
                return true;
            }
            line.push_back(ch);
        }
    }

    bool WriteAll(int socket_fd, const std::string &buffer) {
        std::size_t sent = 0;
        while (sent < buffer.size()) {
            const auto bytes =
                send(socket_fd, buffer.data() + static_cast<long>(sent),
                     buffer.size() - sent, 0);
            if (bytes <= 0) {
                return false;
            }
            sent += static_cast<std::size_t>(bytes);
        }
        return true;
    }

    std::optional<dbms::network::ResponseEnvelope>
    SendRemote(const std::string &host, int port,
               const dbms::network::RequestEnvelope &request) {
        const int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (socket_fd < 0) {
            return std::nullopt;
        }

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(static_cast<uint16_t>(port));
        if (inet_pton(AF_INET, host.c_str(), &address.sin_addr) != 1) {
            close(socket_fd);
            return std::nullopt;
        }

        if (connect(socket_fd, reinterpret_cast<sockaddr *>(&address),
                    sizeof(address)) != 0) {
            close(socket_fd);
            return std::nullopt;
        }

        const auto wire_request = dbms::network::SerializeRequest(request);
        if (!WriteAll(socket_fd, wire_request)) {
            close(socket_fd);
            return std::nullopt;
        }
        std::string wire_response;
        if (!ReadLine(socket_fd, wire_response)) {
            close(socket_fd);
            return std::nullopt;
        }
        close(socket_fd);

        dbms::network::ResponseEnvelope response;
        if (!dbms::network::DeserializeResponse(wire_response, response)) {
            return std::nullopt;
        }
        return response;
    }

    int ExecuteAndPrint(dbms::core::DbmsEngine &engine,
                        dbms::core::SessionContext &session,
                        const std::string &sql,
                        std::size_t statement_index) {
        auto result = engine.ExecuteSql(session, sql);
        if (!result.ok()) {
            std::cout << "error[" << statement_index
                      << "]: "
                      << dbms::common::FormatErrorContract(*result.error, sql)
                      << "\n";
            return 1;
        }
        if (!result.value->column_names.empty()) {
            PrintSelectJson(*result.value);
        } else {
            std::cout << result.value->message << "\n";
        }
        return 0;
    }

    int ExecuteAndPrintRemote(const std::string &host, int port,
                              const std::string &client_id,
                              const std::string &jwt_token,
                              const std::string &sql,
                              std::size_t statement_index) {
        const dbms::network::RequestEnvelope request{
            .client_id = client_id,
            .jwt_token = jwt_token,
            .payload = sql,
        };
        const auto response = SendRemote(host, port, request);
        if (!response.has_value()) {
            std::cout << "error[" << statement_index
                      << "]: type=NETWORK_ERROR code=6 message=failed to reach server sql=\""
                      << dbms::common::EscapeJsonText(sql) << "\"\n";
            return 1;
        }
        std::cout << response->payload << "\n";
        return response->status_code == 200 ? 0 : 1;
    }

} // namespace

// this file boots the cli client and routes commands through the dbms engine.
int main(int argc, char **argv) {
    dbms::core::DbmsEngine engine("./data");
    dbms::core::SessionContext session;
    session.client_id = "cli";
    bool use_remote = false;
    bool run_demo = false;
    std::string remote_host = "127.0.0.1";
    int remote_port = 4545;
    std::string jwt_token;
    int arg_index = 1;

    if (argc >= 2 && std::string(argv[1]) == "--help") {
        PrintUsage(argv[0]);
        return 0;
    }

    while (arg_index < argc) {
        const std::string arg = argv[arg_index];
        if (arg == "--help") {
            PrintUsage(argv[0]);
            return 0;
        }
        if (arg == "--demo") {
            run_demo = true;
            ++arg_index;
            continue;
        }
        if (arg == "--server") {
            if (arg_index + 1 >= argc) {
                std::cout << "error: --server requires host:port\n";
                return 1;
            }
            const auto endpoint = ParseEndpoint(argv[arg_index + 1]);
            if (!endpoint.has_value()) {
                std::cout << "error: invalid endpoint, expected host:port\n";
                return 1;
            }
            use_remote = true;
            remote_host = endpoint->first;
            remote_port = endpoint->second;
            arg_index += 2;
            continue;
        }
        if (arg == "--jwt") {
            if (arg_index + 1 >= argc) {
                std::cout << "error: --jwt requires token\n";
                return 1;
            }
            jwt_token = argv[arg_index + 1];
            arg_index += 2;
            continue;
        }
        break;
    }

    if (run_demo) {
        int exit_code = 0;
        std::size_t statement_index = 1;
        for (const auto &statement : BuildDemoStatements()) {
            int rc = 0;
            if (use_remote) {
                rc = ExecuteAndPrintRemote(remote_host, remote_port, "cli", jwt_token,
                                           statement, statement_index);
            } else {
                rc = ExecuteAndPrint(engine, session, statement, statement_index);
            }
            if (rc != 0) {
                exit_code = 1;
            }
            ++statement_index;
        }
        return exit_code;
    }

    // batch mode: ./dbms_cli script.sql
    if (argc == arg_index + 1) {
        std::ifstream input_file(argv[arg_index]);
        if (!input_file.is_open()) {
            std::cout << "error: cannot open script file: " << argv[arg_index]
                      << "\n";
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
            int rc = 0;
            if (use_remote) {
                rc = ExecuteAndPrintRemote(remote_host, remote_port, "cli", jwt_token,
                                           statement, statement_index);
            } else {
                rc = ExecuteAndPrint(engine, session, statement, statement_index);
            }
            if (rc != 0) {
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
            if (use_remote) {
                ExecuteAndPrintRemote(remote_host, remote_port, "cli", jwt_token,
                                      statement,
                                      statement_index);
            } else {
                ExecuteAndPrint(engine, session, statement, statement_index);
            }
            ++statement_index;
        }

        if (executable_count < statements.size()) {
            buffer = statements.back();
        } else {
            buffer.clear();
        }
    }

    return 0;
}
