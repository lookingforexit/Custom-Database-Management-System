#include "server/entrypoint.hpp"

// this file routes requests through the dbms engine and preserves session
// state.
#include "common/error_contract.hpp"
#include "common/uuid.hpp"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <functional>
#include <fstream>
#include <iomanip>
#include <optional>
#include <sstream>
#include <thread>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
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

        std::string SanitizeLogField(std::string value) {
            for (char &ch : value) {
                if (ch == '\n' || ch == '\r' || ch == '\t') {
                    ch = ' ';
                }
            }
            return value;
        }

    } // namespace

    EntrypointServer::EntrypointServer(std::string root_path)
        : engine_(root_path), access_log_path_(root_path + "/access.log") {
        std::filesystem::create_directories(root_path);
        job_queue_.Start([this](const std::string &sql) -> std::pair<int, std::string> {
            auto parsed = parser_.Parse(sql);
            if (!parsed.ok()) {
                return {400, common::FormatErrorContract(*parsed.error, sql)};
            }

            auto &session = GetOrCreateSession("__async_worker__");
            auto result = engine_.ExecuteSql(session, sql);
            if (!result.ok()) {
                return {400, common::FormatErrorContract(*result.error, sql)};
            }
            if (IsSelectStatement(*parsed.value)) {
                return {200, QueryResultToJson(*result.value)};
            }
            return {200, result.value->message};
        });
    }

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
        const auto started_at = std::chrono::steady_clock::now();
        auto finalize = [&](network::ResponseEnvelope response) {
            const auto finished_at = std::chrono::steady_clock::now();
            const auto latency_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                        finished_at - started_at)
                                        .count();
            if (response.status_code >= 400) {
                telemetry_.RecordFailure(static_cast<double>(latency_ms));
            } else {
                telemetry_.RecordSuccess(static_cast<double>(latency_ms));
            }
            WriteAccessLog(request, response, latency_ms);
            return response;
        };

        network::ResponseEnvelope cluster_response;
        if (ParseClusterCommand(request.payload, cluster_response)) {
            return finalize(cluster_response);
        }
        network::ResponseEnvelope async_response;
        if (ParseAsyncCommand(request, async_response)) {
            return finalize(async_response);
        }
        network::ResponseEnvelope telemetry_response;
        if (ParseTelemetryCommand(request.payload, telemetry_response)) {
            return finalize(telemetry_response);
        }
        network::ResponseEnvelope auth_response;
        if (ParseAuthCommand(request, auth_response)) {
            return finalize(auth_response);
        }

        auto parsed = parser_.Parse(request.payload);
        if (!parsed.ok()) {
            return finalize({.status_code = 400,
                             .payload = common::FormatErrorContract(*parsed.error,
                                                                    request.payload)});
        }
        const auto &statement = *parsed.value;

        auto &session = GetOrCreateSession(request.client_id);
        session.client_id = request.client_id;
        if (access_controller_.HasAccounts()) {
            const auto user_id =
                access_controller_.ValidateTokenAndGetUser(request.jwt_token);
            if (!user_id.has_value()) {
                return finalize({.status_code = 401,
                                 .payload = "type=AUTHORIZATION_ERROR code=5 message=invalid or expired jwt token sql=\"\""});
            }
            session.user_id = *user_id;
            const auto required_permission = RequiredPermission(statement);
            if (required_permission.has_value() &&
                !access_controller_.Authorize(*user_id, session.current_database,
                                              *required_permission)) {
                return finalize({.status_code = 403,
                                 .payload = "type=AUTHORIZATION_ERROR code=5 message=permission denied sql=\"\""});
            }
        }

        std::vector<StorageNodeEndpoint> nodes_snapshot;
        {
            std::lock_guard<std::mutex> lock(nodes_mutex_);
            nodes_snapshot = storage_nodes_;
        }

        if (!nodes_snapshot.empty()) {
            if (IsMutatingStatement(statement)) {
                auto tx_result = ExecuteTwoPhaseCommit(nodes_snapshot, request);
                if (!tx_result.has_value()) {
                    return finalize({.status_code = 502,
                                     .payload = "type=NETWORK_ERROR code=6 message=cluster 2pc failed sql=\"\""});
                }
                return finalize(*tx_result);
            }

            if (ShouldBroadcast(statement)) {
                std::string merged_unused;
                auto broadcast_result =
                    BroadcastToStorageNodes(nodes_snapshot, request, false,
                                            merged_unused);
                if (!broadcast_result.has_value()) {
                    return finalize({.status_code = 502,
                                     .payload = "type=NETWORK_ERROR code=6 message=cluster broadcast failed sql=\"\""});
                }
                return finalize(*broadcast_result);
            }

            if (IsSelectStatement(statement) &&
                !RouteNodeIndex(statement, nodes_snapshot).has_value()) {
                std::string merged_json;
                auto fanout =
                    BroadcastToStorageNodes(nodes_snapshot, request, true, merged_json);
                if (!fanout.has_value()) {
                    return finalize({.status_code = 502,
                                     .payload = "type=NETWORK_ERROR code=6 message=cluster fan-out failed sql=\"\""});
                }
                return finalize({.status_code = 200, .payload = merged_json});
            }

            const auto node_index = RouteNodeIndex(statement, nodes_snapshot);
            if (!node_index.has_value()) {
                return finalize({.status_code = 502,
                                 .payload = "type=NETWORK_ERROR code=6 message=unable to route request sql=\"\""});
            }
            auto forwarded =
                ForwardToStorageNode(nodes_snapshot[*node_index], request);
            if (forwarded.has_value()) {
                return finalize(*forwarded);
            }
            return finalize({.status_code = 502,
                             .payload = "type=NETWORK_ERROR code=6 message=target storage node unavailable sql=\"\""});
        }

        auto result = engine_.ExecuteSql(session, request.payload);
        if (!result.ok()) {
            return finalize({
                .status_code = 400,
                .payload = common::FormatErrorContract(*result.error, request.payload),
            });
        }

        if (IsSelectStatement(statement)) {
            return finalize(
                {.status_code = 200, .payload = QueryResultToJson(*result.value)});
        }
        return finalize({.status_code = 200, .payload = result.value->message});
    }

    void EntrypointServer::WriteAccessLog(
        const network::RequestEnvelope &request,
        const network::ResponseEnvelope &response,
        std::int64_t latency_ms) const {
        const auto now = std::chrono::system_clock::now();
        const std::time_t time_value = std::chrono::system_clock::to_time_t(now);
        std::tm utc_tm{};
#if defined(_WIN32)
        gmtime_s(&utc_tm, &time_value);
#else
        gmtime_r(&time_value, &utc_tm);
#endif

        std::ostringstream timestamp;
        timestamp << std::put_time(&utc_tm, "%Y-%m-%dT%H:%M:%SZ");

        std::lock_guard<std::mutex> lock(access_log_mutex_);
        std::ofstream out(access_log_path_, std::ios::app);
        if (!out.is_open()) {
            return;
        }
        out << "ts=" << timestamp.str() << "\tclient_id="
            << SanitizeLogField(request.client_id) << "\tstatus="
            << response.status_code << "\tlatency_ms=" << latency_ms << "\tsql=\""
            << SanitizeLogField(request.payload) << "\"\n";
    }

    bool EntrypointServer::ParseClusterCommand(
        const std::string &payload, network::ResponseEnvelope &response) {
        const std::string prepare_prefix = "CLUSTER PREPARE_TX ";
        if (payload.rfind(prepare_prefix, 0) == 0) {
            const auto rest = payload.substr(prepare_prefix.size());
            const auto split = rest.find(' ');
            if (split == std::string::npos) {
                response = {.status_code = 400,
                            .payload = "type=VALIDATION_ERROR code=3 message=missing tx id/sql sql=\"\""};
                return true;
            }
            const std::string tx_id = rest.substr(0, split);
            const std::string sql = rest.substr(split + 1);
            if (tx_id.empty() || sql.empty()) {
                response = {.status_code = 400,
                            .payload = "type=VALIDATION_ERROR code=3 message=missing tx id/sql sql=\"\""};
                return true;
            }
            auto parsed = parser_.Parse(sql);
            if (!parsed.ok()) {
                response = {.status_code = 400,
                            .payload = common::FormatErrorContract(*parsed.error, sql)};
                return true;
            }
            if (!IsMutatingStatement(*parsed.value)) {
                response = {.status_code = 400,
                            .payload = "type=VALIDATION_ERROR code=3 message=PREPARE_TX requires mutating sql sql=\"\""};
                return true;
            }
            std::lock_guard<std::mutex> lock(pending_transactions_mutex_);
            pending_cluster_transactions_[tx_id] =
                PendingClusterTransaction{.sql = sql, .client_id = "__cluster_tx__"};
            response = {.status_code = 200, .payload = "prepared"};
            return true;
        }
        const std::string commit_prefix = "CLUSTER COMMIT_TX ";
        if (payload.rfind(commit_prefix, 0) == 0) {
            const std::string tx_id = payload.substr(commit_prefix.size());
            if (tx_id.empty()) {
                response = {.status_code = 400,
                            .payload = "type=VALIDATION_ERROR code=3 message=missing tx id sql=\"\""};
                return true;
            }
            PendingClusterTransaction pending;
            {
                std::lock_guard<std::mutex> lock(pending_transactions_mutex_);
                auto it = pending_cluster_transactions_.find(tx_id);
                if (it == pending_cluster_transactions_.end()) {
                    response = {.status_code = 404,
                                .payload = "type=NOT_FOUND code=7 message=unknown tx id sql=\"\""};
                    return true;
                }
                pending = it->second;
                pending_cluster_transactions_.erase(it);
            }
            auto &session = GetOrCreateSession(pending.client_id);
            auto result = engine_.ExecuteSql(session, pending.sql);
            if (!result.ok()) {
                response = {.status_code = 400,
                            .payload =
                                common::FormatErrorContract(*result.error, pending.sql)};
                return true;
            }
            response = {.status_code = 200, .payload = result.value->message};
            return true;
        }
        const std::string abort_prefix = "CLUSTER ABORT_TX ";
        if (payload.rfind(abort_prefix, 0) == 0) {
            const std::string tx_id = payload.substr(abort_prefix.size());
            if (tx_id.empty()) {
                response = {.status_code = 400,
                            .payload = "type=VALIDATION_ERROR code=3 message=missing tx id sql=\"\""};
                return true;
            }
            std::lock_guard<std::mutex> lock(pending_transactions_mutex_);
            pending_cluster_transactions_.erase(tx_id);
            response = {.status_code = 200, .payload = "aborted"};
            return true;
        }

        const auto tokens = SplitWhitespace(payload);
        if (tokens.size() < 2 || ToUpper(tokens[0]) != "CLUSTER") {
            return false;
        }

        const auto command = ToUpper(tokens[1]);
        if (command == "PING") {
            response = {.status_code = 200, .payload = "pong"};
            return true;
        }
        if (command == "LIST_NODES") {
            std::ostringstream output;
            std::lock_guard<std::mutex> lock(nodes_mutex_);
            output << "nodes=" << storage_nodes_.size();
            for (std::size_t index = 0; index < storage_nodes_.size(); ++index) {
                output << " [" << index << "] " << storage_nodes_[index].host << ":"
                       << storage_nodes_[index].port
                       << " managed=" << (storage_nodes_[index].managed ? "true" : "false")
                       << " fail_count=" << storage_nodes_[index].fail_count;
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
                const bool managed = tokens.size() >= 4 && ToUpper(tokens[3]) == "MANAGED";
                std::lock_guard<std::mutex> lock(nodes_mutex_);
                const auto exists = std::any_of(
                    storage_nodes_.begin(), storage_nodes_.end(),
                    [&](const StorageNodeEndpoint &node) {
                        return node.host == endpoint->first &&
                               node.port == endpoint->second;
                    });
                if (!exists) {
                    storage_nodes_.push_back(
                        {.host = endpoint->first,
                         .port = endpoint->second,
                         .managed = managed,
                         .fail_count = 0});
                }
                response = {.status_code = 200, .payload = "node added"};
                return true;
            }
            std::lock_guard<std::mutex> lock(nodes_mutex_);
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

    bool EntrypointServer::ParseAsyncCommand(
        const network::RequestEnvelope &request, network::ResponseEnvelope &response) {
        const auto tokens = SplitWhitespace(request.payload);
        if (tokens.size() < 2 || ToUpper(tokens[0]) != "ASYNC") {
            return false;
        }
        const auto command = ToUpper(tokens[1]);

        if (command == "SUBMIT") {
            if (tokens.size() < 3) {
                response = {.status_code = 400,
                            .payload = "type=VALIDATION_ERROR code=3 message=missing sql for ASYNC SUBMIT sql=\"\""};
                return true;
            }
            const std::string prefix = "ASYNC SUBMIT ";
            if (request.payload.size() <= prefix.size()) {
                response = {.status_code = 400,
                            .payload = "type=VALIDATION_ERROR code=3 message=missing sql for ASYNC SUBMIT sql=\"\""};
                return true;
            }
            runtime::JobRecord job;
            job.job_id = common::UuidGenerator::NewGuidV4();
            job.sql = request.payload.substr(prefix.size());
            job.status = "QUEUED";
            job_queue_.Enqueue(job);
            response = {.status_code = 202, .payload = "job_id=" + job.job_id};
            return true;
        }

        if ((command == "STATUS" || command == "RESULT") && tokens.size() >= 3) {
            const std::string job_id = tokens[2];
            auto job = job_queue_.Find(job_id);
            if (!job.has_value()) {
                response = {.status_code = 404,
                            .payload = "type=NOT_FOUND code=7 message=job not found sql=\"\""};
                return true;
            }
            if (command == "STATUS") {
                response = {.status_code = 200,
                            .payload = "job_id=" + job->job_id + " status=" + job->status};
                return true;
            }
            if (job->status == "QUEUED" || job->status == "RUNNING") {
                response = {.status_code = 202,
                            .payload =
                                "job_id=" + job->job_id + " status=" + job->status};
                return true;
            }
            response = {.status_code = job->result_code, .payload = job->result_payload};
            return true;
        }

        response = {.status_code = 400,
                    .payload = "type=VALIDATION_ERROR code=3 message=invalid ASYNC command sql=\"\""};
        return true;
    }

    bool EntrypointServer::ParseTelemetryCommand(
        const std::string &payload, network::ResponseEnvelope &response) {
        const auto tokens = SplitWhitespace(payload);
        if (tokens.size() < 2 || ToUpper(tokens[0]) != "TELEMETRY") {
            return false;
        }
        const auto command = ToUpper(tokens[1]);
        if (command != "SNAPSHOT") {
            response = {.status_code = 400,
                        .payload = "type=VALIDATION_ERROR code=3 message=invalid TELEMETRY command sql=\"\""};
            return true;
        }

        const auto snapshot = telemetry_.Snapshot();
        std::ostringstream output;
        output << "current_rps=" << snapshot.current_rps
               << " average_rps_10m=" << snapshot.average_rps_10m
               << " max_rps_10m=" << snapshot.max_rps_10m
               << " average_latency_10s_ms=" << snapshot.average_latency_10s_ms
               << " error_rate_1m=" << snapshot.error_rate_1m;
        response = {.status_code = 200, .payload = output.str()};
        return true;
    }

    bool EntrypointServer::ParseAuthCommand(
        const network::RequestEnvelope &request, network::ResponseEnvelope &response) {
        const auto tokens = SplitWhitespace(request.payload);
        if (tokens.size() < 2 || ToUpper(tokens[0]) != "AUTH") {
            return false;
        }
        const auto command = ToUpper(tokens[1]);
        if (command == "REGISTER" && tokens.size() >= 4) {
            const std::string group = tokens.size() >= 5 ? tokens[4] : "admin";
            std::string error_message;
            if (!access_controller_.RegisterAccount(tokens[2], tokens[3], group,
                                                    error_message)) {
                response = {.status_code = 400,
                            .payload = "type=VALIDATION_ERROR code=3 message=" +
                                           error_message + " sql=\"\""};
                return true;
            }
            response = {.status_code = 200, .payload = "registered"};
            return true;
        }
        if (command == "LOGIN" && tokens.size() >= 4) {
            std::string error_message;
            const auto token =
                access_controller_.LoginAndIssueToken(tokens[2], tokens[3], error_message);
            if (!token.has_value()) {
                response = {.status_code = 401,
                            .payload = "type=AUTHORIZATION_ERROR code=5 message=" +
                                           error_message + " sql=\"\""};
                return true;
            }
            response = {.status_code = 200, .payload = "token=" + *token};
            return true;
        }
        response = {.status_code = 400,
                    .payload = "type=VALIDATION_ERROR code=3 message=invalid AUTH command sql=\"\""};
        return true;
    }

    std::optional<catalog::Permission>
    EntrypointServer::RequiredPermission(const parser::Statement &statement) const {
        if (std::holds_alternative<parser::SelectStatement>(statement) ||
            std::holds_alternative<parser::UseDatabaseStatement>(statement)) {
            return catalog::Permission::kRead;
        }
        if (std::holds_alternative<parser::InsertStatement>(statement) ||
            std::holds_alternative<parser::UpdateStatement>(statement) ||
            std::holds_alternative<parser::DeleteStatement>(statement) ||
            std::holds_alternative<parser::RevertStatement>(statement) ||
            std::holds_alternative<parser::BeginTransactionStatement>(statement) ||
            std::holds_alternative<parser::CommitTransactionStatement>(statement) ||
            std::holds_alternative<parser::RollbackTransactionStatement>(statement)) {
            return catalog::Permission::kWrite;
        }
        if (std::holds_alternative<parser::CreateDatabaseStatement>(statement) ||
            std::holds_alternative<parser::CreateTableStatement>(statement)) {
            return catalog::Permission::kCreateTable;
        }
        if (std::holds_alternative<parser::DropTableStatement>(statement)) {
            return catalog::Permission::kDropTable;
        }
        if (std::holds_alternative<parser::DropDatabaseStatement>(statement)) {
            return catalog::Permission::kDropDatabase;
        }
        return std::nullopt;
    }

    std::optional<std::size_t>
    EntrypointServer::RouteNodeIndex(
        const parser::Statement &statement,
        const std::vector<StorageNodeEndpoint> &nodes) const {
        if (nodes.empty()) {
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
        return hash_value % nodes.size();
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

    bool EntrypointServer::IsMutatingStatement(const parser::Statement &statement) const {
        return !std::holds_alternative<parser::SelectStatement>(statement) &&
               !std::holds_alternative<parser::UseDatabaseStatement>(statement) &&
               !std::holds_alternative<parser::UnknownStatement>(statement);
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
    EntrypointServer::BroadcastToStorageNodes(
        const std::vector<StorageNodeEndpoint> &nodes,
        const network::RequestEnvelope &request,
                                              bool collect_json,
                                              std::string &merged_json) const {
        std::vector<std::string> json_parts;
        for (const auto &node : nodes) {
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
    EntrypointServer::ExecuteTwoPhaseCommit(
        const std::vector<StorageNodeEndpoint> &nodes,
        const network::RequestEnvelope &request) const {
        const std::string tx_id = common::UuidGenerator::NewGuidV4();

        std::vector<StorageNodeEndpoint> prepared_nodes;
        for (const auto &node : nodes) {
            network::RequestEnvelope prepare_request = request;
            prepare_request.payload =
                "CLUSTER PREPARE_TX " + tx_id + " " + request.payload;
            auto prepare_response = ForwardToStorageNode(node, prepare_request);
            if (!prepare_response.has_value() || prepare_response->status_code != 200) {
                for (const auto &prepared : prepared_nodes) {
                    network::RequestEnvelope abort_request = request;
                    abort_request.payload = "CLUSTER ABORT_TX " + tx_id;
                    (void)ForwardToStorageNode(prepared, abort_request);
                }
                return std::nullopt;
            }
            prepared_nodes.push_back(node);
        }

        for (const auto &node : prepared_nodes) {
            network::RequestEnvelope commit_request = request;
            commit_request.payload = "CLUSTER COMMIT_TX " + tx_id;
            auto commit_response = ForwardToStorageNode(node, commit_request);
            if (!commit_response.has_value() || commit_response->status_code != 200) {
                return std::nullopt;
            }
        }
        return network::ResponseEnvelope{.status_code = 200, .payload = "2pc committed"};
    }

    bool EntrypointServer::RunHeartbeatCycle() {
        std::vector<std::size_t> dead_indices;
        std::vector<StorageNodeEndpoint> nodes_snapshot;
        {
            std::lock_guard<std::mutex> lock(nodes_mutex_);
            nodes_snapshot = storage_nodes_;
        }

        const network::RequestEnvelope ping_request{
            .client_id = "__cluster_heartbeat__",
            .jwt_token = "",
            .payload = "CLUSTER PING",
        };

        for (std::size_t index = 0; index < nodes_snapshot.size(); ++index) {
            auto response = ForwardToStorageNode(nodes_snapshot[index], ping_request);
            const bool healthy =
                response.has_value() && response->status_code == 200 &&
                response->payload == "pong";
            if (!healthy) {
                dead_indices.push_back(index);
            }
        }

        bool changed = false;
        std::lock_guard<std::mutex> lock(nodes_mutex_);
        for (std::size_t index = 0; index < storage_nodes_.size(); ++index) {
            const bool is_dead = std::find(dead_indices.begin(), dead_indices.end(),
                                           index) != dead_indices.end();
            if (!is_dead) {
                storage_nodes_[index].fail_count = 0;
                continue;
            }
            ++storage_nodes_[index].fail_count;
            if (storage_nodes_[index].managed && storage_nodes_[index].fail_count >= 3) {
                if (RestartManagedNode(storage_nodes_[index])) {
                    storage_nodes_[index].fail_count = 0;
                    changed = true;
                }
            }
        }
        return changed;
    }

    bool EntrypointServer::RestartManagedNode(StorageNodeEndpoint &node) const {
        if (!node.managed) {
            return false;
        }
        if (node.host != "127.0.0.1" && node.host != "localhost") {
            return false;
        }

        const pid_t child_pid = fork();
        if (child_pid < 0) {
            return false;
        }
        if (child_pid == 0) {
            const std::string port_string = std::to_string(node.port);
            execlp("dbms_storage_node", "dbms_storage_node", port_string.c_str(),
                   nullptr);
            _exit(1);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        return true;
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
