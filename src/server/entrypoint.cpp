#include "server/entrypoint.hpp"

// this file routes requests through the dbms engine and preserves session
// state.
#include "common/error_contract.hpp"
#include <algorithm>
#include <cctype>
#include <functional>
#include <optional>
#include <sstream>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace dbms::server {

    namespace {

        std::string ToUpper(std::string value) {
            for (char &ch : value) {
                ch = static_cast<char>(
                    std::toupper(static_cast<unsigned char>(ch)));
            }
            return value;
        }

        std::vector<std::string> SplitWhitespace(const std::string &value) {
            std::istringstream input(value);
            std::vector<std::string> parts;
            std::string token;
            while (input >> token) {
                parts.push_back(token);
            }
            return parts;
        }

        std::optional<std::pair<std::string, int>>
        ParseEndpoint(const std::string &endpoint) {
            const auto colon_position = endpoint.rfind(':');
            if (colon_position == std::string::npos || colon_position == 0 ||
                colon_position + 1 >= endpoint.size()) {
                return std::nullopt;
            }
            try {
                const std::string host = endpoint.substr(0, colon_position);
                const int port = std::stoi(endpoint.substr(colon_position + 1));
                if (port <= 0 || port > 65535) {
                    return std::nullopt;
                }
                return std::make_pair(host, port);
            } catch (...) {
                return std::nullopt;
            }
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

    } // namespace

    EntrypointServer::EntrypointServer(std::string root_path)
        : engine_(std::move(root_path)) {}

    core::SessionContext &
    EntrypointServer::GetOrCreateSession(const std::string &client_id) {
        auto [it, inserted] = sessions_.try_emplace(client_id);
        if (inserted) {
            it->second.client_id = client_id;
        }
        return it->second;
    }

    network::ResponseEnvelope
    EntrypointServer::HandleRequest(const network::RequestEnvelope &request) {
        network::ResponseEnvelope cluster_response;
        if (ParseClusterCommand(request.payload, cluster_response)) {
            return cluster_response;
        }

        auto parsed = parser_.Parse(request.payload);
        if (!parsed.ok()) {
            return {.status_code = 400,
                    .payload =
                        common::FormatErrorContract(*parsed.error, request.payload)};
        }
        const auto &statement = *parsed.value;

        auto &session = GetOrCreateSession(request.client_id);
        session.client_id = request.client_id;

        if (!storage_nodes_.empty()) {
            if (ShouldBroadcast(statement)) {
                std::string merged_unused;
                auto broadcast_result =
                    BroadcastToStorageNodes(request, false, merged_unused);
                if (!broadcast_result.has_value()) {
                    return {.status_code = 502,
                            .payload = "type=NETWORK_ERROR code=6 message=cluster broadcast failed sql=\"\""};
                }
                return *broadcast_result;
            }

            if (IsSelectStatement(statement) && !RouteNodeIndex(statement).has_value()) {
                std::string merged_json;
                auto fanout =
                    BroadcastToStorageNodes(request, true, merged_json);
                if (!fanout.has_value()) {
                    return {.status_code = 502,
                            .payload = "type=NETWORK_ERROR code=6 message=cluster fan-out failed sql=\"\""};
                }
                return {.status_code = 200, .payload = merged_json};
            }

            const auto node_index = RouteNodeIndex(statement);
            if (!node_index.has_value()) {
                return {.status_code = 502,
                        .payload = "type=NETWORK_ERROR code=6 message=unable to route request sql=\"\""};
            }
            auto forwarded =
                ForwardToStorageNode(storage_nodes_[*node_index], request);
            if (forwarded.has_value()) {
                return *forwarded;
            }
            return {.status_code = 502,
                    .payload = "type=NETWORK_ERROR code=6 message=target storage node unavailable sql=\"\""};
        }

        auto result = engine_.ExecuteSql(session, request.payload);
        if (!result.ok()) {
            return {
                .status_code = 400,
                .payload = common::FormatErrorContract(*result.error, request.payload),
            };
        }

        if (IsSelectStatement(statement)) {
            return {.status_code = 200, .payload = QueryResultToJson(*result.value)};
        }
        return {.status_code = 200, .payload = result.value->message};
    }

    bool EntrypointServer::ParseClusterCommand(
        const std::string &payload, network::ResponseEnvelope &response) {
        const auto tokens = SplitWhitespace(payload);
        if (tokens.size() < 2 || ToUpper(tokens[0]) != "CLUSTER") {
            return false;
        }

        const auto command = ToUpper(tokens[1]);
        if (command == "LIST_NODES") {
            std::ostringstream output;
            output << "nodes=" << storage_nodes_.size();
            for (std::size_t index = 0; index < storage_nodes_.size(); ++index) {
                output << " [" << index << "] " << storage_nodes_[index].host << ":"
                       << storage_nodes_[index].port;
            }
            response = {.status_code = 200, .payload = output.str()};
            return true;
        }
        if ((command == "ADD_NODE" || command == "REMOVE_NODE") &&
            tokens.size() >= 3) {
            auto endpoint = ParseEndpoint(tokens[2]);
            if (!endpoint.has_value()) {
                response = {.status_code = 400,
                            .payload = "type=VALIDATION_ERROR code=3 message=invalid endpoint sql=\"\""};
                return true;
            }
            if (command == "ADD_NODE") {
                const auto exists = std::any_of(
                    storage_nodes_.begin(), storage_nodes_.end(),
                    [&](const StorageNodeEndpoint &node) {
                        return node.host == endpoint->first &&
                               node.port == endpoint->second;
                    });
                if (!exists) {
                    storage_nodes_.push_back(
                        {.host = endpoint->first, .port = endpoint->second});
                }
                response = {.status_code = 200, .payload = "node added"};
                return true;
            }
            storage_nodes_.erase(
                std::remove_if(storage_nodes_.begin(), storage_nodes_.end(),
                               [&](const StorageNodeEndpoint &node) {
                                   return node.host == endpoint->first &&
                                          node.port == endpoint->second;
                               }),
                storage_nodes_.end());
            response = {.status_code = 200, .payload = "node removed"};
            return true;
        }

        response = {.status_code = 400,
                    .payload = "type=VALIDATION_ERROR code=3 message=invalid CLUSTER command sql=\"\""};
        return true;
    }

    std::optional<std::size_t>
    EntrypointServer::RouteNodeIndex(const parser::Statement &statement) const {
        if (storage_nodes_.empty()) {
            return std::nullopt;
        }
        if (const auto *select = std::get_if<parser::SelectStatement>(&statement);
            select != nullptr) {
            if (!select->where.has_value()) {
                return std::nullopt;
            }
        }
        if (const auto *update = std::get_if<parser::UpdateStatement>(&statement);
            update != nullptr) {
            if (!update->where.has_value()) {
                return std::nullopt;
            }
        }
        if (const auto *delete_statement =
                std::get_if<parser::DeleteStatement>(&statement);
            delete_statement != nullptr) {
            if (!delete_statement->where.has_value()) {
                return std::nullopt;
            }
        }
        const auto table_name = ExtractTargetTableName(statement);
        if (!table_name.has_value()) {
            return std::nullopt;
        }
        const std::size_t hash_value = std::hash<std::string>{}(*table_name);
        return hash_value % storage_nodes_.size();
    }

    std::optional<std::string>
    EntrypointServer::ExtractTargetTableName(const parser::Statement &statement) const {
        if (const auto *create_table =
                std::get_if<parser::CreateTableStatement>(&statement);
            create_table != nullptr) {
            return create_table->table_name.object_name;
        }
        if (const auto *drop_table =
                std::get_if<parser::DropTableStatement>(&statement);
            drop_table != nullptr) {
            return drop_table->table_name.object_name;
        }
        if (const auto *insert = std::get_if<parser::InsertStatement>(&statement);
            insert != nullptr) {
            return insert->table_name.object_name;
        }
        if (const auto *update = std::get_if<parser::UpdateStatement>(&statement);
            update != nullptr) {
            return update->table_name.object_name;
        }
        if (const auto *delete_statement =
                std::get_if<parser::DeleteStatement>(&statement);
            delete_statement != nullptr) {
            return delete_statement->table_name.object_name;
        }
        if (const auto *select = std::get_if<parser::SelectStatement>(&statement);
            select != nullptr) {
            return select->table_name.object_name;
        }
        if (const auto *revert = std::get_if<parser::RevertStatement>(&statement);
            revert != nullptr) {
            return revert->table_name.object_name;
        }
        return std::nullopt;
    }

    bool EntrypointServer::ShouldBroadcast(const parser::Statement &statement) const {
        if (std::holds_alternative<parser::CreateDatabaseStatement>(statement) ||
            std::holds_alternative<parser::DropDatabaseStatement>(statement) ||
            std::holds_alternative<parser::UseDatabaseStatement>(statement) ||
            std::holds_alternative<parser::CreateTableStatement>(statement) ||
            std::holds_alternative<parser::DropTableStatement>(statement) ||
            std::holds_alternative<parser::RevertStatement>(statement) ||
            std::holds_alternative<parser::BeginTransactionStatement>(statement) ||
            std::holds_alternative<parser::CommitTransactionStatement>(statement) ||
            std::holds_alternative<parser::RollbackTransactionStatement>(statement)) {
            return true;
        }
        if (const auto *update = std::get_if<parser::UpdateStatement>(&statement);
            update != nullptr) {
            return !update->where.has_value();
        }
        if (const auto *delete_statement =
                std::get_if<parser::DeleteStatement>(&statement);
            delete_statement != nullptr) {
            return !delete_statement->where.has_value();
        }
        return false;
    }

    bool EntrypointServer::IsSelectStatement(const parser::Statement &statement) const {
        return std::holds_alternative<parser::SelectStatement>(statement);
    }

    std::string
    EntrypointServer::QueryResultToJson(const execution::QueryResult &result) const {
        std::ostringstream output;
        output << "[";
        for (std::size_t row_index = 0; row_index < result.rows.size(); ++row_index) {
            if (row_index != 0) {
                output << ", ";
            }
            output << "{";
            const auto &row = result.rows[row_index];
            for (std::size_t column_index = 0;
                 column_index < result.column_names.size() &&
                 column_index < row.values.size();
                 ++column_index) {
                if (column_index != 0) {
                    output << ", ";
                }
                output << "\"" << common::EscapeJsonText(result.column_names[column_index])
                       << "\": ";
                const auto &value = row.values[column_index];
                if (std::holds_alternative<std::monostate>(value)) {
                    output << "null";
                } else if (std::holds_alternative<std::int64_t>(value)) {
                    output << std::get<std::int64_t>(value);
                } else {
                    output << "\""
                           << common::EscapeJsonText(std::get<std::string>(value))
                           << "\"";
                }
            }
            output << "}";
        }
        output << "]";
        return output.str();
    }

    std::optional<network::ResponseEnvelope>
    EntrypointServer::BroadcastToStorageNodes(const network::RequestEnvelope &request,
                                              bool collect_json,
                                              std::string &merged_json) const {
        std::vector<std::string> json_parts;
        for (const auto &node : storage_nodes_) {
            auto response = ForwardToStorageNode(node, request);
            if (!response.has_value()) {
                return std::nullopt;
            }
            if (response->status_code != 200) {
                return response;
            }
            if (collect_json) {
                json_parts.push_back(response->payload);
            }
        }
        if (!collect_json) {
            return network::ResponseEnvelope{.status_code = 200,
                                             .payload = "broadcast ok"};
        }
        std::ostringstream merged;
        merged << "[";
        bool first = true;
        for (const auto &part : json_parts) {
            const auto trimmed_start = part.find_first_not_of(" \t\r\n");
            const auto trimmed_end = part.find_last_not_of(" \t\r\n");
            if (trimmed_start == std::string::npos || trimmed_end == std::string::npos) {
                continue;
            }
            const std::string trimmed =
                part.substr(trimmed_start, trimmed_end - trimmed_start + 1);
            if (trimmed == "[]") {
                continue;
            }
            if (trimmed.size() < 2 || trimmed.front() != '[' || trimmed.back() != ']') {
                continue;
            }
            const std::string inner = trimmed.substr(1, trimmed.size() - 2);
            if (inner.empty()) {
                continue;
            }
            if (!first) {
                merged << ", ";
            }
            merged << inner;
            first = false;
        }
        merged << "]";
        merged_json = merged.str();
        return network::ResponseEnvelope{.status_code = 200, .payload = merged_json};
    }

    std::optional<network::ResponseEnvelope>
    EntrypointServer::ForwardToStorageNode(
        const StorageNodeEndpoint &node,
        const network::RequestEnvelope &request) const {
        const int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (socket_fd < 0) {
            return std::nullopt;
        }

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(static_cast<uint16_t>(node.port));
        if (inet_pton(AF_INET, node.host.c_str(), &address.sin_addr) != 1) {
            close(socket_fd);
            return std::nullopt;
        }
        if (connect(socket_fd, reinterpret_cast<sockaddr *>(&address),
                    sizeof(address)) != 0) {
            close(socket_fd);
            return std::nullopt;
        }

        const auto wire_request = network::SerializeRequest(request);
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
        network::ResponseEnvelope response;
        if (!network::DeserializeResponse(wire_response, response)) {
            return std::nullopt;
        }
        return response;
    }

} // namespace dbms::server
