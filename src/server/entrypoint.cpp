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
#include <regex>
#include <sstream>
#include <thread>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

namespace dbms::server {

    std::unordered_map<std::string, EntrypointServer *>
        EntrypointServer::local_node_registry_{};
    std::mutex EntrypointServer::local_node_registry_mutex_{};

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
                if (ch == '\n' || ch == '\r' || ch == '\t' ||
                    static_cast<unsigned char>(ch) < 32U) {
                    ch = ' ';
                }
            }
            return value;
        }

        std::string FormatUtcTimestamp(
            std::chrono::system_clock::time_point time_point) {
            const auto seconds =
                std::chrono::time_point_cast<std::chrono::seconds>(time_point);
            const auto millis =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    time_point - seconds)
                    .count();
            const std::time_t time_value =
                std::chrono::system_clock::to_time_t(time_point);
            std::tm utc_tm{};
#if defined(_WIN32)
            gmtime_s(&utc_tm, &time_value);
#else
            gmtime_r(&time_value, &utc_tm);
#endif
            std::ostringstream timestamp;
            timestamp << std::put_time(&utc_tm, "%Y-%m-%dT%H:%M:%S") << "."
                      << std::setw(3) << std::setfill('0') << millis << "Z";
            return timestamp.str();
        }

        std::string EscapeClusterField(const std::string &value) {
            std::string escaped;
            escaped.reserve(value.size());
            for (char ch : value) {
                if (ch == '\\' || ch == '\t' || ch == '\n') {
                    escaped.push_back('\\');
                }
                escaped.push_back(ch);
            }
            return escaped;
        }

        std::string UnescapeClusterField(const std::string &value) {
            std::string unescaped;
            unescaped.reserve(value.size());
            bool escaped = false;
            for (char ch : value) {
                if (escaped) {
                    unescaped.push_back(ch);
                    escaped = false;
                } else if (ch == '\\') {
                    escaped = true;
                } else {
                    unescaped.push_back(ch);
                }
            }
            return unescaped;
        }

        std::vector<std::string> SplitByTab(const std::string &line) {
            std::vector<std::string> parts;
            std::string current;
            bool escaped = false;
            for (char ch : line) {
                if (escaped) {
                    current.push_back(ch);
                    escaped = false;
                    continue;
                }
                if (ch == '\\') {
                    escaped = true;
                    current.push_back(ch);
                    continue;
                }
                if (ch == '\t') {
                    parts.push_back(current);
                    current.clear();
                    continue;
                }
                current.push_back(ch);
            }
            parts.push_back(current);
            return parts;
        }

        std::optional<common::Value> DecodeClusterValue(const std::string &encoded) {
            if (encoded == "Null") {
                return std::monostate{};
            }
            if (encoded.rfind("Int:", 0) == 0) {
                try {
                    std::size_t consumed = 0;
                    const auto parsed = std::stoll(encoded.substr(4), &consumed);
                    if (consumed != encoded.size() - 4) {
                        return std::nullopt;
                    }
                    return static_cast<std::int64_t>(parsed);
                } catch (...) {
                    return std::nullopt;
                }
            }
            if (encoded.rfind("String:", 0) == 0) {
                return UnescapeClusterField(encoded.substr(7));
            }
            return std::nullopt;
        }

    } // namespace

    EntrypointServer::EntrypointServer(std::string root_path,
                                       std::string local_endpoint)
        : engine_(root_path), access_controller_(root_path),
          access_log_path_(root_path + "/access.log"),
          local_endpoint_(std::move(local_endpoint)) {
        std::filesystem::create_directories(root_path);
        if (!local_endpoint_.empty()) {
            std::scoped_lock lock(local_node_registry_mutex_);
            local_node_registry_[local_endpoint_] = this;
        }
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

    EntrypointServer::~EntrypointServer() {
        if (!local_endpoint_.empty()) {
            std::scoped_lock lock(local_node_registry_mutex_);
            const auto it = local_node_registry_.find(local_endpoint_);
            if (it != local_node_registry_.end() && it->second == this) {
                local_node_registry_.erase(it);
            }
        }
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
        const auto started_at_utc = std::chrono::system_clock::now();
        const auto started_at = std::chrono::steady_clock::now();
        std::ostringstream handler_id_builder;
        handler_id_builder << "handler-" << std::setw(6) << std::setfill('0')
                           << next_handler_id_.fetch_add(1);
        const std::string handler_id = handler_id_builder.str();
        auto finalize = [&](network::ResponseEnvelope response) {
            const auto finished_at = std::chrono::steady_clock::now();
            const auto finished_at_utc = std::chrono::system_clock::now();
            const auto latency_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                        finished_at - started_at)
                                        .count();
            if (response.status_code >= 400) {
                telemetry_.RecordFailure(static_cast<double>(latency_ms));
            } else {
                telemetry_.RecordSuccess(static_cast<double>(latency_ms));
            }
            WriteAccessLog(request, response, handler_id, started_at_utc,
                           finished_at_utc, latency_ms);
            return response;
        };

        network::ResponseEnvelope cluster_response;
        if (ParseClusterCommand(request, cluster_response)) {
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
            const auto authorization_database_name =
                ResolveAuthorizationDatabaseName(statement, session);
            if (required_permission.has_value() &&
                !access_controller_.Authorize(*user_id, authorization_database_name,
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
            if (IsMetadataStatement(statement)) {
                auto local_result = engine_.ExecuteSql(session, request.payload);
                if (!local_result.ok()) {
                    return finalize({
                        .status_code = 400,
                        .payload =
                            common::FormatErrorContract(*local_result.error,
                                                        request.payload),
                    });
                }
                if (ShouldBroadcast(statement)) {
                    if (IsMutatingStatement(statement)) {
                        auto tx_result =
                            ExecuteTwoPhaseCommit(nodes_snapshot, request);
                        if (!tx_result.has_value()) {
                            return finalize({.status_code = 502,
                                             .payload = "type=NETWORK_ERROR code=6 message=cluster metadata broadcast failed sql=\"\""});
                        }
                    } else {
                        std::string merged_unused;
                        auto broadcast_result =
                            BroadcastToStorageNodes(nodes_snapshot, request, false,
                                                    merged_unused);
                        if (!broadcast_result.has_value()) {
                            return finalize({.status_code = 502,
                                             .payload = "type=NETWORK_ERROR code=6 message=cluster metadata broadcast failed sql=\"\""});
                        }
                    }
                }
                if (IsSelectStatement(statement)) {
                    return finalize({.status_code = 200,
                                     .payload =
                                         QueryResultToJson(*local_result.value)});
                }
                return finalize(
                    {.status_code = 200, .payload = local_result.value->message});
            }

            if (const auto *insert =
                    std::get_if<parser::InsertStatement>(&statement);
                insert != nullptr) {
                const auto routed_sql =
                    SplitInsertByShard(*insert, nodes_snapshot, session);
                if (!routed_sql.has_value()) {
                    return finalize({.status_code = 502,
                                     .payload = "type=NETWORK_ERROR code=6 message=unable to shard insert sql=\"\""});
                }
                std::vector<std::pair<StorageNodeEndpoint, std::string>> prepared;
                for (const auto &[node_index, sql] : *routed_sql) {
                    prepared.push_back({nodes_snapshot[node_index], sql});
                }
                auto tx_result = ExecuteTwoPhaseCommit(prepared, request);
                if (!tx_result.has_value()) {
                    return finalize({.status_code = 502,
                                     .payload = "type=NETWORK_ERROR code=6 message=cluster sharded insert failed sql=\"\""});
                }
                return finalize(*tx_result);
            }

            if (ShouldBroadcast(statement)) {
                if (IsMutatingStatement(statement)) {
                    auto tx_result = ExecuteTwoPhaseCommit(nodes_snapshot, request);
                    if (!tx_result.has_value()) {
                        return finalize({.status_code = 502,
                                         .payload = "type=NETWORK_ERROR code=6 message=cluster 2pc failed sql=\"\""});
                    }
                    return finalize(*tx_result);
                }
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
                !RouteNodeIndex(statement, nodes_snapshot, session).has_value()) {
                std::string merged_json;
                auto fanout =
                    BroadcastToStorageNodes(nodes_snapshot, request, true, merged_json);
                if (!fanout.has_value()) {
                    return finalize({.status_code = 502,
                                     .payload = "type=NETWORK_ERROR code=6 message=cluster fan-out failed sql=\"\""});
                }
                return finalize({.status_code = 200, .payload = merged_json});
            }

            const auto node_index =
                RouteNodeIndex(statement, nodes_snapshot, session);
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
        const std::string &handler_id,
        std::chrono::system_clock::time_point started_at_utc,
        std::chrono::system_clock::time_point finished_at_utc,
        std::int64_t latency_ms) const {
        std::lock_guard<std::mutex> lock(access_log_mutex_);
        std::ofstream out(access_log_path_, std::ios::app);
        if (!out.is_open()) {
            return;
        }
        out << "start=" << FormatUtcTimestamp(started_at_utc)
            << "\tfinish=" << FormatUtcTimestamp(finished_at_utc)
            << "\tclient_id=" << SanitizeLogField(request.client_id)
            << "\thandler_id=" << SanitizeLogField(handler_id)
            << "\tstatus_code=" << response.status_code
            << "\tlatency_ms=" << latency_ms
            << "\tsql=\"" << SanitizeLogField(request.payload) << "\"\n";
    }

    bool EntrypointServer::ParseClusterCommand(
        const network::RequestEnvelope &request, network::ResponseEnvelope &response) {
        const std::string &payload = request.payload;
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
            auto &cluster_session = GetOrCreateSession(request.client_id);
            std::lock_guard<std::mutex> lock(pending_transactions_mutex_);
            pending_cluster_transactions_[tx_id] =
                PendingClusterTransaction{
                    .sql = sql,
                    .client_id = request.client_id,
                    .current_database = cluster_session.current_database,
                };
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
            session.current_database = pending.current_database;
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
        if (command == "DUMP_TABLE" && tokens.size() >= 4) {
            core::SessionContext dump_session;
            dump_session.client_id = "__cluster_dump__";
            auto use_result = engine_.ExecuteSql(
                dump_session, "USE " + tokens[2] + ";");
            if (!use_result.ok()) {
                response = {.status_code = 400,
                            .payload = common::FormatErrorContract(*use_result.error,
                                                                   tokens[2])};
                return true;
            }
            auto select_result =
                engine_.ExecuteSql(dump_session, "SELECT * FROM " + tokens[3] + ";");
            if (!select_result.ok()) {
                response = {.status_code = 400,
                            .payload = common::FormatErrorContract(
                                *select_result.error, tokens[3])};
                return true;
            }
            std::ostringstream output;
            output << "ROWS";
            for (const auto &row : select_result.value->rows) {
                output << "\nROW";
                for (const auto &value : row.values) {
                    if (std::holds_alternative<std::monostate>(value)) {
                        output << "\tNull";
                    } else if (std::holds_alternative<std::int64_t>(value)) {
                        output << "\tInt:" << std::get<std::int64_t>(value);
                    } else {
                        output << "\tString:"
                               << EscapeClusterField(common::AsString(value));
                    }
                }
            }
            response = {.status_code = 200, .payload = output.str()};
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
                std::vector<StorageNodeEndpoint> before_nodes;
                std::vector<StorageNodeEndpoint> after_nodes;
                bool added = false;
                {
                    std::lock_guard<std::mutex> lock(nodes_mutex_);
                    before_nodes = storage_nodes_;
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
                        added = true;
                    }
                    after_nodes = storage_nodes_;
                }
                if (added && !before_nodes.empty()) {
                    (void)SyncMetadataToNodes(
                        {StorageNodeEndpoint{.host = endpoint->first,
                                             .port = endpoint->second,
                                             .managed = managed,
                                             .fail_count = 0}});
                }
                response = {.status_code = 200, .payload = "node added"};
                return true;
            }
            std::vector<StorageNodeEndpoint> before_nodes;
            std::vector<StorageNodeEndpoint> after_nodes;
            bool removed = false;
            {
                std::lock_guard<std::mutex> lock(nodes_mutex_);
                before_nodes = storage_nodes_;
                auto it = std::find_if(storage_nodes_.begin(), storage_nodes_.end(),
                                       [&](const StorageNodeEndpoint &node) {
                                           return node.host == endpoint->first &&
                                                  node.port == endpoint->second;
                                       });
                if (it != storage_nodes_.end()) {
                    storage_nodes_.erase(it);
                    removed = true;
                }
                after_nodes = storage_nodes_;
            }
            if (removed && !after_nodes.empty()) {
                std::vector<StorageNodeEndpoint> removed_source;
                for (const auto &node : before_nodes) {
                    if (node.host == endpoint->first &&
                        node.port == endpoint->second) {
                        removed_source.push_back(node);
                    }
                }
                if (!RebalanceClusterData(removed_source, after_nodes, false)) {
                    response = {.status_code = 502,
                                .payload = "type=NETWORK_ERROR code=6 message=rebalance after remove failed sql=\"\""};
                    return true;
                }
            }
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
        if (command == "LOCAL") {
            response = {.status_code = 200,
                        .payload = SerializeLocalTelemetry(telemetry_.Snapshot())};
            return true;
        }
        if (command != "SNAPSHOT") {
            response = {.status_code = 400,
                        .payload = "type=VALIDATION_ERROR code=3 message=invalid TELEMETRY command sql=\"\""};
            return true;
        }

        const auto local_snapshot = telemetry_.Snapshot();
        std::vector<runtime::TelemetrySnapshot> remote_snapshots;
        std::size_t nodes_total = 0;
        std::size_t nodes_available = 0;
        {
            std::lock_guard<std::mutex> lock(nodes_mutex_);
            nodes_total = storage_nodes_.size();
            for (const auto &node : storage_nodes_) {
                const network::RequestEnvelope telemetry_request{
                    .client_id = "__telemetry__",
                    .jwt_token = "",
                    .payload = "TELEMETRY LOCAL",
                };
                auto node_response = ForwardToStorageNode(node, telemetry_request);
                if (!node_response.has_value() || node_response->status_code != 200) {
                    continue;
                }
                auto parsed = ParseLocalTelemetry(node_response->payload);
                if (!parsed.has_value()) {
                    continue;
                }
                ++nodes_available;
                remote_snapshots.push_back(std::move(*parsed));
            }
        }
        const auto snapshot =
            AggregateTelemetry(local_snapshot, remote_snapshots);
        std::ostringstream output;
        output << "current_rps=" << snapshot.current_rps
               << " average_rps_10m=" << snapshot.average_rps_10m
               << " max_rps_10m=" << snapshot.max_rps_10m
               << " average_latency_10s_ms=" << snapshot.average_latency_10s_ms
               << " error_count_1m=" << snapshot.error_count_1m
               << " error_rate_1m=" << snapshot.error_rate_1m
               << " nodes_total=" << (nodes_total + 1)
               << " nodes_available=" << (nodes_available + 1);
        response = {.status_code = 200, .payload = output.str()};
        return true;
    }

    std::string EntrypointServer::SerializeLocalTelemetry(
        const runtime::TelemetrySnapshot &snapshot) const {
        std::ostringstream output;
        output << "current_rps=" << snapshot.current_rps
               << "\taverage_rps_10m=" << snapshot.average_rps_10m
               << "\tmax_rps_10m=" << snapshot.max_rps_10m
               << "\taverage_latency_10s_ms=" << snapshot.average_latency_10s_ms
               << "\terror_count_1m=" << snapshot.error_count_1m
               << "\terror_rate_1m=" << snapshot.error_rate_1m
               << "\trequests_1m=" << snapshot.requests_1m
               << "\tlatency_samples_10s=" << snapshot.latency_samples_10s
               << "\tlatency_sum_10s_ms=" << snapshot.latency_sum_10s_ms
               << "\tbuckets=";
        bool first = true;
        for (const auto &[second_key, count] : snapshot.requests_per_second_10m) {
            if (!first) {
                output << ",";
            }
            output << second_key << ":" << count;
            first = false;
        }
        return output.str();
    }

    std::optional<runtime::TelemetrySnapshot>
    EntrypointServer::ParseLocalTelemetry(const std::string &payload) const {
        runtime::TelemetrySnapshot snapshot;
        const auto parts = SplitByTab(payload);
        for (const auto &part : parts) {
            const auto split = part.find('=');
            if (split == std::string::npos) {
                continue;
            }
            const std::string key = part.substr(0, split);
            const std::string value = part.substr(split + 1);
            try {
                if (key == "current_rps") {
                    snapshot.current_rps = std::stod(value);
                } else if (key == "average_rps_10m") {
                    snapshot.average_rps_10m = std::stod(value);
                } else if (key == "max_rps_10m") {
                    snapshot.max_rps_10m = std::stod(value);
                } else if (key == "average_latency_10s_ms") {
                    snapshot.average_latency_10s_ms = std::stod(value);
                } else if (key == "error_count_1m") {
                    snapshot.error_count_1m =
                        static_cast<std::uint64_t>(std::stoull(value));
                } else if (key == "error_rate_1m") {
                    snapshot.error_rate_1m = std::stod(value);
                } else if (key == "requests_1m") {
                    snapshot.requests_1m =
                        static_cast<std::uint64_t>(std::stoull(value));
                } else if (key == "latency_samples_10s") {
                    snapshot.latency_samples_10s =
                        static_cast<std::uint64_t>(std::stoull(value));
                } else if (key == "latency_sum_10s_ms") {
                    snapshot.latency_sum_10s_ms = std::stod(value);
                } else if (key == "buckets" && !value.empty()) {
                    std::istringstream bucket_input(value);
                    std::string bucket;
                    while (std::getline(bucket_input, bucket, ',')) {
                        const auto colon = bucket.find(':');
                        if (colon == std::string::npos) {
                            return std::nullopt;
                        }
                        snapshot.requests_per_second_10m.emplace(
                            std::stoll(bucket.substr(0, colon)),
                            static_cast<std::uint64_t>(
                                std::stoull(bucket.substr(colon + 1))));
                    }
                }
            } catch (...) {
                return std::nullopt;
            }
        }
        return snapshot;
    }

    runtime::TelemetrySnapshot EntrypointServer::AggregateTelemetry(
        const runtime::TelemetrySnapshot &local_snapshot,
        const std::vector<runtime::TelemetrySnapshot> &remote_snapshots) const {
        runtime::TelemetrySnapshot merged = local_snapshot;
        for (const auto &snapshot : remote_snapshots) {
            merged.error_count_1m += snapshot.error_count_1m;
            merged.requests_1m += snapshot.requests_1m;
            merged.latency_samples_10s += snapshot.latency_samples_10s;
            merged.latency_sum_10s_ms += snapshot.latency_sum_10s_ms;
            for (const auto &[second_key, count] : snapshot.requests_per_second_10m) {
                merged.requests_per_second_10m[second_key] += count;
            }
        }
        const auto now_second =
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count();
        merged.current_rps = static_cast<double>(
            merged.requests_per_second_10m[now_second]);
        std::uint64_t total_requests_10m = 0;
        std::uint64_t max_bucket = 0;
        for (const auto &[second_key, count] : merged.requests_per_second_10m) {
            (void)second_key;
            total_requests_10m += count;
            max_bucket = std::max(max_bucket, count);
        }
        merged.average_rps_10m = static_cast<double>(total_requests_10m) / 600.0;
        merged.max_rps_10m = static_cast<double>(max_bucket);
        merged.average_latency_10s_ms =
            merged.latency_samples_10s == 0
                ? 0.0
                : (merged.latency_sum_10s_ms /
                   static_cast<double>(merged.latency_samples_10s));
        merged.error_rate_1m =
            merged.requests_1m == 0
                ? 0.0
                : (static_cast<double>(merged.error_count_1m) /
                   static_cast<double>(merged.requests_1m));
        return merged;
    }

    bool EntrypointServer::ParseAuthCommand(
        const network::RequestEnvelope &request, network::ResponseEnvelope &response) {
        const auto tokens = SplitWhitespace(request.payload);
        if (tokens.size() < 2 || ToUpper(tokens[0]) != "AUTH") {
            return false;
        }
        const auto command = ToUpper(tokens[1]);
        auto require_admin = [&]() -> bool {
            if (!access_controller_.HasAccounts()) {
                response = {.status_code = 403,
                            .payload = "type=AUTHORIZATION_ERROR code=5 message=admin required sql=\"\""};
                return false;
            }
            const auto user_id =
                access_controller_.ValidateTokenAndGetUser(request.jwt_token);
            if (!user_id.has_value()) {
                response = {.status_code = 401,
                            .payload = "type=AUTHORIZATION_ERROR code=5 message=invalid or expired jwt token sql=\"\""};
                return false;
            }
            if (!access_controller_.IsAdmin(*user_id)) {
                response = {.status_code = 403,
                            .payload = "type=AUTHORIZATION_ERROR code=5 message=admin required sql=\"\""};
                return false;
            }
            return true;
        };
        if (command == "REGISTER" && tokens.size() >= 4) {
            const std::string group =
                tokens.size() >= 5 ? tokens[4] : (access_controller_.HasAccounts()
                                                      ? "user"
                                                      : "admin");
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
        if (command == "CREATE_GROUP" && tokens.size() >= 3) {
            if (!require_admin()) {
                return true;
            }
            std::string error_message;
            if (!access_controller_.CreateGroup(tokens[2], error_message)) {
                response = {.status_code = 400,
                            .payload = "type=VALIDATION_ERROR code=3 message=" +
                                           error_message + " sql=\"\""};
                return true;
            }
            response = {.status_code = 200, .payload = "group created"};
            return true;
        }
        if (command == "ADD_USER_GROUP" && tokens.size() >= 4) {
            if (!require_admin()) {
                return true;
            }
            std::string error_message;
            if (!access_controller_.AddUserToGroup(tokens[2], tokens[3],
                                                   error_message)) {
                response = {.status_code = 400,
                            .payload = "type=VALIDATION_ERROR code=3 message=" +
                                           error_message + " sql=\"\""};
                return true;
            }
            response = {.status_code = 200, .payload = "user added to group"};
            return true;
        }
        if ((command == "GRANT_DEFAULT" || command == "REVOKE_DEFAULT") &&
            tokens.size() >= 4) {
            if (!require_admin()) {
                return true;
            }
            const auto permission =
                catalog::AccessController::ParsePermissionName(tokens[3]);
            if (!permission.has_value()) {
                response = {.status_code = 400,
                            .payload = "type=VALIDATION_ERROR code=3 message=invalid permission sql=\"\""};
                return true;
            }
            std::string error_message;
            const bool ok = command == "GRANT_DEFAULT"
                                ? access_controller_.GrantDefault(tokens[2],
                                                                  *permission,
                                                                  error_message)
                                : access_controller_.RevokeDefault(tokens[2],
                                                                   *permission,
                                                                   error_message);
            if (!ok) {
                response = {.status_code = 400,
                            .payload = "type=VALIDATION_ERROR code=3 message=" +
                                           error_message + " sql=\"\""};
                return true;
            }
            response = {.status_code = 200, .payload = "default grant updated"};
            return true;
        }
        if ((command == "GRANT_GROUP" || command == "REVOKE_GROUP") &&
            tokens.size() >= 5) {
            if (!require_admin()) {
                return true;
            }
            const auto permission =
                catalog::AccessController::ParsePermissionName(tokens[4]);
            if (!permission.has_value()) {
                response = {.status_code = 400,
                            .payload = "type=VALIDATION_ERROR code=3 message=invalid permission sql=\"\""};
                return true;
            }
            std::string error_message;
            const bool ok = command == "GRANT_GROUP"
                                ? access_controller_.GrantGroup(tokens[2], tokens[3],
                                                                *permission,
                                                                error_message)
                                : access_controller_.RevokeGroup(tokens[2], tokens[3],
                                                                 *permission,
                                                                 error_message);
            if (!ok) {
                response = {.status_code = 400,
                            .payload = "type=VALIDATION_ERROR code=3 message=" +
                                           error_message + " sql=\"\""};
                return true;
            }
            response = {.status_code = 200, .payload = "group grant updated"};
            return true;
        }
        if ((command == "GRANT_USER" || command == "REVOKE_USER") &&
            tokens.size() >= 5) {
            if (!require_admin()) {
                return true;
            }
            const auto permission =
                catalog::AccessController::ParsePermissionName(tokens[4]);
            if (!permission.has_value()) {
                response = {.status_code = 400,
                            .payload = "type=VALIDATION_ERROR code=3 message=invalid permission sql=\"\""};
                return true;
            }
            std::string error_message;
            const bool ok = command == "GRANT_USER"
                                ? access_controller_.GrantUser(tokens[2], tokens[3],
                                                               *permission,
                                                               error_message)
                                : access_controller_.RevokeUser(tokens[2], tokens[3],
                                                                *permission,
                                                                error_message);
            if (!ok) {
                response = {.status_code = 400,
                            .payload = "type=VALIDATION_ERROR code=3 message=" +
                                           error_message + " sql=\"\""};
                return true;
            }
            response = {.status_code = 200, .payload = "user grant updated"};
            return true;
        }
        if (command == "WHOAMI") {
            if (!access_controller_.HasAccounts()) {
                response = {.status_code = 200, .payload = "anonymous"};
                return true;
            }
            const auto user_id =
                access_controller_.ValidateTokenAndGetUser(request.jwt_token);
            if (!user_id.has_value()) {
                response = {.status_code = 401,
                            .payload = "type=AUTHORIZATION_ERROR code=5 message=invalid or expired jwt token sql=\"\""};
                return true;
            }
            const auto groups = access_controller_.UserGroups(*user_id);
            std::ostringstream payload;
            payload << "user=" << *user_id << " groups=";
            for (std::size_t index = 0; index < groups.size(); ++index) {
                if (index != 0) {
                    payload << ",";
                }
                payload << groups[index];
            }
            response = {.status_code = 200, .payload = payload.str()};
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

    std::string EntrypointServer::ResolveAuthorizationDatabaseName(
        const parser::Statement &statement,
        const core::SessionContext &session) const {
        if (const auto *use =
                std::get_if<parser::UseDatabaseStatement>(&statement);
            use != nullptr) {
            return use->database_name;
        }
        if (const auto *create_database =
                std::get_if<parser::CreateDatabaseStatement>(&statement);
            create_database != nullptr) {
            return create_database->database_name;
        }
        if (const auto *drop_database =
                std::get_if<parser::DropDatabaseStatement>(&statement);
            drop_database != nullptr) {
            return drop_database->database_name;
        }
        if (const auto *create_table =
                std::get_if<parser::CreateTableStatement>(&statement);
            create_table != nullptr && create_table->table_name.database_name.has_value()) {
            return *create_table->table_name.database_name;
        }
        if (const auto *drop_table =
                std::get_if<parser::DropTableStatement>(&statement);
            drop_table != nullptr && drop_table->table_name.database_name.has_value()) {
            return *drop_table->table_name.database_name;
        }
        if (const auto *insert = std::get_if<parser::InsertStatement>(&statement);
            insert != nullptr && insert->table_name.database_name.has_value()) {
            return *insert->table_name.database_name;
        }
        if (const auto *update = std::get_if<parser::UpdateStatement>(&statement);
            update != nullptr && update->table_name.database_name.has_value()) {
            return *update->table_name.database_name;
        }
        if (const auto *delete_statement =
                std::get_if<parser::DeleteStatement>(&statement);
            delete_statement != nullptr &&
            delete_statement->table_name.database_name.has_value()) {
            return *delete_statement->table_name.database_name;
        }
        if (const auto *select = std::get_if<parser::SelectStatement>(&statement);
            select != nullptr && select->table_name.database_name.has_value()) {
            return *select->table_name.database_name;
        }
        if (const auto *revert = std::get_if<parser::RevertStatement>(&statement);
            revert != nullptr && revert->table_name.database_name.has_value()) {
            return *revert->table_name.database_name;
        }
        return session.current_database;
    }

    std::optional<std::string> EntrypointServer::ResolveClusterDatabaseName(
        const parser::QualifiedName &table_name,
        const core::SessionContext &session) const {
        if (table_name.database_name.has_value()) {
            return table_name.database_name;
        }
        if (!session.current_database.empty()) {
            return session.current_database;
        }
        return std::nullopt;
    }

    std::optional<catalog::TableSchema> EntrypointServer::ResolveClusterTableSchema(
        const parser::QualifiedName &table_name,
        const core::SessionContext &session) const {
        const auto database_name =
            ResolveClusterDatabaseName(table_name, session);
        if (!database_name.has_value()) {
            return std::nullopt;
        }
        const auto &databases = engine_.runtime_state().databases;
        const auto database_it = databases.find(*database_name);
        if (database_it == databases.end()) {
            return std::nullopt;
        }
        const auto table_it =
            database_it->second.tables.find(table_name.object_name);
        if (table_it == database_it->second.tables.end()) {
            return std::nullopt;
        }
        return table_it->second.schema;
    }

    std::optional<std::string>
    EntrypointServer::ResolveShardKeyColumn(const catalog::TableSchema &schema) const {
        for (const auto &column : schema.columns) {
            if (column.constraint == catalog::ColumnConstraint::kIndexed &&
                column.type == common::ValueType::kInt64) {
                return column.name;
            }
        }
        for (const auto &column : schema.columns) {
            if (column.constraint == catalog::ColumnConstraint::kIndexed) {
                return column.name;
            }
        }
        return std::nullopt;
    }

    std::optional<common::Value> EntrypointServer::TryExtractShardKeyValue(
        const parser::Expression &expr, const std::string &shard_key_column) const {
        if (const auto *logical = std::get_if<parser::LogicalExpression>(&expr.node);
            logical != nullptr) {
            if (logical->op == parser::LogicalOperator::kOr) {
                return std::nullopt;
            }
            auto left = TryExtractShardKeyValue(*logical->left, shard_key_column);
            if (left.has_value()) {
                return left;
            }
            return TryExtractShardKeyValue(*logical->right, shard_key_column);
        }
        const auto *comparison =
            std::get_if<parser::BinaryComparisonExpression>(&expr.node);
        if (comparison == nullptr ||
            comparison->op != parser::ComparisonOperator::kEqual) {
            return std::nullopt;
        }

        auto extract = [&](const parser::Expression &column_expr,
                           const parser::Expression &value_expr)
            -> std::optional<common::Value> {
            const auto *column =
                std::get_if<parser::ColumnReferenceExpression>(&column_expr.node);
            const auto *literal =
                std::get_if<parser::LiteralExpression>(&value_expr.node);
            if (column == nullptr || literal == nullptr) {
                return std::nullopt;
            }
            if (column->column_name != shard_key_column) {
                return std::nullopt;
            }
            return literal->value;
        };

        if (auto value = extract(*comparison->left, *comparison->right);
            value.has_value()) {
            return value;
        }
        return extract(*comparison->right, *comparison->left);
    }

    EntrypointServer::ShardRoutingDecision EntrypointServer::RouteValueToNode(
        const common::Value &value,
        const std::vector<StorageNodeEndpoint> &nodes) const {
        if (nodes.empty()) {
            return {};
        }
        const auto hash_value = common::GetValueType(value) == common::ValueType::kInt64
                                    ? std::hash<std::int64_t>{}(
                                          std::get<std::int64_t>(value))
                                    : std::hash<std::string>{}(
                                          common::AsString(value));
        return {.node_index = hash_value % nodes.size(), .by_shard_key = true};
    }

    std::optional<std::size_t>
    EntrypointServer::RouteNodeIndex(
        const parser::Statement &statement,
        const std::vector<StorageNodeEndpoint> &nodes,
        const core::SessionContext &session) const {
        if (nodes.empty()) {
            return std::nullopt;
        }
        auto evaluate_route = [&](const parser::QualifiedName &table_name,
                                  const std::optional<parser::Expression> &where)
            -> std::optional<std::size_t> {
            if (!where.has_value()) {
                return std::nullopt;
            }
            const auto schema = ResolveClusterTableSchema(table_name, session);
            if (!schema.has_value()) {
                return std::nullopt;
            }
            const auto shard_key = ResolveShardKeyColumn(*schema);
            if (!shard_key.has_value()) {
                return std::nullopt;
            }
            const auto shard_value =
                TryExtractShardKeyValue(*where, *shard_key);
            if (!shard_value.has_value()) {
                return std::nullopt;
            }
            return RouteValueToNode(*shard_value, nodes).node_index;
        };
        if (const auto *select = std::get_if<parser::SelectStatement>(&statement);
            select != nullptr) {
            return evaluate_route(select->table_name, select->where);
        }
        if (const auto *update = std::get_if<parser::UpdateStatement>(&statement);
            update != nullptr) {
            return evaluate_route(update->table_name, update->where);
        }
        if (const auto *delete_statement =
                std::get_if<parser::DeleteStatement>(&statement);
            delete_statement != nullptr) {
            return evaluate_route(delete_statement->table_name,
                                  delete_statement->where);
        }
        if (const auto *revert = std::get_if<parser::RevertStatement>(&statement);
            revert != nullptr) {
            return std::nullopt;
        }
        return std::nullopt;
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

    bool EntrypointServer::IsMetadataStatement(
        const parser::Statement &statement) const {
        return std::holds_alternative<parser::CreateDatabaseStatement>(statement) ||
               std::holds_alternative<parser::DropDatabaseStatement>(statement) ||
               std::holds_alternative<parser::UseDatabaseStatement>(statement) ||
               std::holds_alternative<parser::CreateTableStatement>(statement) ||
               std::holds_alternative<parser::DropTableStatement>(statement);
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
    EntrypointServer::RenderLiteralSql(const common::Value &value) const {
        if (std::holds_alternative<std::monostate>(value)) {
            return "NULL";
        }
        if (std::holds_alternative<std::int64_t>(value)) {
            return std::to_string(std::get<std::int64_t>(value));
        }
        std::string escaped = common::AsString(value);
        std::string quoted;
        quoted.reserve(escaped.size() + 2);
        quoted.push_back('"');
        for (char ch : escaped) {
            if (ch == '\\' || ch == '"') {
                quoted.push_back('\\');
            }
            quoted.push_back(ch);
        }
        quoted.push_back('"');
        return quoted;
    }

    std::string EntrypointServer::RenderInsertSql(
        const parser::QualifiedName &table_name,
        const std::vector<std::string> &column_names,
        const std::vector<std::vector<common::Value>> &rows) const {
        std::ostringstream sql;
        sql << "INSERT INTO ";
        if (table_name.database_name.has_value()) {
            sql << *table_name.database_name << ".";
        }
        sql << table_name.object_name << " (";
        for (std::size_t index = 0; index < column_names.size(); ++index) {
            if (index != 0) {
                sql << ", ";
            }
            sql << column_names[index];
        }
        sql << ") VALUES ";
        for (std::size_t row_index = 0; row_index < rows.size(); ++row_index) {
            if (row_index != 0) {
                sql << ", ";
            }
            sql << "(";
            for (std::size_t column_index = 0; column_index < rows[row_index].size();
                 ++column_index) {
                if (column_index != 0) {
                    sql << ", ";
                }
                sql << RenderLiteralSql(rows[row_index][column_index]);
            }
            sql << ")";
        }
        sql << ";";
        return sql.str();
    }

    std::string
    EntrypointServer::RenderCreateTableSql(const catalog::TableSchema &schema) const {
        std::ostringstream sql;
        sql << "CREATE TABLE " << schema.table_name << " (";
        for (std::size_t index = 0; index < schema.columns.size(); ++index) {
            if (index != 0) {
                sql << ", ";
            }
            const auto &column = schema.columns[index];
            sql << column.name << " "
                << (column.type == common::ValueType::kInt64 ? "INT" : "STRING");
            if (column.constraint == catalog::ColumnConstraint::kNotNull) {
                sql << " NOT NULL";
            }
            if (column.constraint == catalog::ColumnConstraint::kIndexed) {
                sql << " INDEXED";
            }
            if (column.default_value.has_value()) {
                sql << " DEFAULT " << RenderLiteralSql(*column.default_value);
            }
        }
        sql << ");";
        return sql.str();
    }

    std::optional<std::unordered_map<std::size_t, std::string>>
    EntrypointServer::SplitInsertByShard(
        const parser::InsertStatement &insert,
        const std::vector<StorageNodeEndpoint> &nodes,
        const core::SessionContext &session) const {
        const auto schema = ResolveClusterTableSchema(insert.table_name, session);
        if (!schema.has_value()) {
            return std::nullopt;
        }
        const auto shard_key = ResolveShardKeyColumn(*schema);
        if (!shard_key.has_value()) {
            return std::nullopt;
        }
        auto shard_column_it =
            std::find(insert.column_names.begin(), insert.column_names.end(),
                      *shard_key);
        if (shard_column_it == insert.column_names.end()) {
            return std::nullopt;
        }
        const auto shard_column_index = static_cast<std::size_t>(
            std::distance(insert.column_names.begin(), shard_column_it));
        std::unordered_map<std::size_t, std::vector<std::vector<common::Value>>>
            rows_by_node;
        parser::QualifiedName qualified_table_name = insert.table_name;
        qualified_table_name.database_name =
            ResolveClusterDatabaseName(insert.table_name, session);
        if (!qualified_table_name.database_name.has_value()) {
            return std::nullopt;
        }
        for (const auto &row_exprs : insert.rows) {
            if (shard_column_index >= row_exprs.size()) {
                return std::nullopt;
            }
            std::vector<common::Value> row_values;
            row_values.reserve(row_exprs.size());
            for (const auto &expr : row_exprs) {
                const auto *literal =
                    std::get_if<parser::LiteralExpression>(&expr.node);
                if (literal == nullptr) {
                    return std::nullopt;
                }
                row_values.push_back(literal->value);
            }
            const auto route =
                RouteValueToNode(row_values[shard_column_index], nodes);
            rows_by_node[route.node_index].push_back(std::move(row_values));
        }
        std::unordered_map<std::size_t, std::string> sql_by_node;
        for (auto &[node_index, rows] : rows_by_node) {
            sql_by_node.emplace(
                node_index,
                RenderInsertSql(qualified_table_name, insert.column_names, rows));
        }
        return sql_by_node;
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
                           << common::EscapeJsonText(common::AsString(value))
                           << "\"";
                }
            }
            output << "}";
        }
        output << "]";
        return output.str();
    }

    std::optional<std::vector<EntrypointServer::DumpedTableRow>>
    EntrypointServer::FetchTableDump(const StorageNodeEndpoint &node,
                                     const std::string &database_name,
                                     const std::string &table_name) const {
        const network::RequestEnvelope request{
            .client_id = "__cluster_rebalance__",
            .jwt_token = "",
            .payload = "CLUSTER DUMP_TABLE " + database_name + " " + table_name,
        };
        auto response = ForwardToStorageNode(node, request);
        if (!response.has_value() || response->status_code != 200) {
            return std::nullopt;
        }
        std::vector<DumpedTableRow> rows;
        std::istringstream input(response->payload);
        std::string line;
        bool first_line = true;
        while (std::getline(input, line)) {
            if (first_line) {
                first_line = false;
                if (line != "ROWS") {
                    return std::nullopt;
                }
                continue;
            }
            if (line.empty()) {
                continue;
            }
            const auto parts = SplitByTab(line);
            if (parts.empty() || parts[0] != "ROW") {
                return std::nullopt;
            }
            DumpedTableRow row;
            for (std::size_t index = 1; index < parts.size(); ++index) {
                auto value = DecodeClusterValue(parts[index]);
                if (!value.has_value()) {
                    return std::nullopt;
                }
                row.values.push_back(*value);
            }
            rows.push_back(std::move(row));
        }
        return rows;
    }

    bool EntrypointServer::SyncMetadataToNodes(
        const std::vector<StorageNodeEndpoint> &target_nodes) const {
        for (const auto &[database_name, database_runtime] :
             engine_.runtime_state().databases) {
            for (const auto &node : target_nodes) {
                network::RequestEnvelope create_db_request{
                    .client_id = "__cluster_rebalance__",
                    .jwt_token = "",
                    .payload = "CREATE DATABASE " + database_name + ";",
                };
                auto create_db_response =
                    ForwardToStorageNode(node, create_db_request);
                if (!create_db_response.has_value() ||
                    (create_db_response->status_code != 200 &&
                     create_db_response->payload.find("already exists") ==
                         std::string::npos)) {
                    return false;
                }
                network::RequestEnvelope use_request{
                    .client_id = "__cluster_rebalance__",
                    .jwt_token = "",
                    .payload = "USE " + database_name + ";",
                };
                auto use_response = ForwardToStorageNode(node, use_request);
                if (!use_response.has_value() || use_response->status_code != 200) {
                    return false;
                }
                for (const auto &[table_name, table_runtime] :
                     database_runtime.tables) {
                    (void)table_name;
                    network::RequestEnvelope create_table_request = use_request;
                    create_table_request.payload =
                        RenderCreateTableSql(table_runtime.schema);
                    auto create_table_response =
                        ForwardToStorageNode(node, create_table_request);
                    if (!create_table_response.has_value() ||
                        (create_table_response->status_code != 200 &&
                         create_table_response->payload.find("already exists") ==
                             std::string::npos)) {
                        return false;
                    }
                }
            }
        }
        return true;
    }

    bool EntrypointServer::RebalanceClusterData(
        const std::vector<StorageNodeEndpoint> &source_nodes,
        const std::vector<StorageNodeEndpoint> &target_nodes,
        bool truncate_targets) const {
        if (source_nodes.empty() || target_nodes.empty()) {
            return true;
        }
        if (!SyncMetadataToNodes(target_nodes)) {
            return false;
        }
        for (const auto &[database_name, database_runtime] :
             engine_.runtime_state().databases) {
            for (const auto &[table_name, table_runtime] : database_runtime.tables) {
                const auto shard_key = ResolveShardKeyColumn(table_runtime.schema);
                if (!shard_key.has_value()) {
                    continue;
                }
                auto shard_it = std::find_if(
                    table_runtime.schema.columns.begin(),
                    table_runtime.schema.columns.end(),
                    [&](const catalog::ColumnDefinition &column) {
                        return column.name == *shard_key;
                    });
                if (shard_it == table_runtime.schema.columns.end()) {
                    continue;
                }
                const auto shard_column_index = static_cast<std::size_t>(
                    std::distance(table_runtime.schema.columns.begin(), shard_it));

                std::vector<DumpedTableRow> all_rows;
                std::unordered_set<std::string> seen_rows;
                for (const auto &node : source_nodes) {
                    auto dumped_rows =
                        FetchTableDump(node, database_name, table_name);
                    if (!dumped_rows.has_value()) {
                        return false;
                    }
                    for (const auto &row : *dumped_rows) {
                        std::ostringstream fingerprint;
                        for (const auto &value : row.values) {
                            fingerprint << RenderLiteralSql(value) << "|";
                        }
                        if (seen_rows.insert(fingerprint.str()).second) {
                            all_rows.push_back(row);
                        }
                    }
                }

                if (truncate_targets) {
                    network::RequestEnvelope truncate_request{
                        .client_id = "__cluster_rebalance__",
                        .jwt_token = "",
                        .payload = "USE " + database_name + ";",
                    };
                    for (const auto &node : target_nodes) {
                        auto use_response =
                            ForwardToStorageNode(node, truncate_request);
                        if (!use_response.has_value() ||
                            use_response->status_code != 200) {
                            return false;
                        }
                        network::RequestEnvelope delete_request = truncate_request;
                        delete_request.payload = "DELETE FROM " + table_name + ";";
                        auto delete_response =
                            ForwardToStorageNode(node, delete_request);
                        if (!delete_response.has_value() ||
                            delete_response->status_code != 200) {
                            return false;
                        }
                    }
                }

                std::unordered_map<std::size_t, std::vector<std::vector<common::Value>>>
                    rows_by_node;
                for (const auto &row : all_rows) {
                    if (shard_column_index >= row.values.size()) {
                        return false;
                    }
                    const auto route =
                        RouteValueToNode(row.values[shard_column_index], target_nodes);
                    rows_by_node[route.node_index].push_back(row.values);
                }
                std::vector<std::pair<StorageNodeEndpoint, std::string>> prepared;
                parser::InsertStatement insert;
                insert.table_name = parser::QualifiedName{
                    .database_name = database_name,
                    .object_name = table_name,
                };
                for (const auto &column : table_runtime.schema.columns) {
                    insert.column_names.push_back(column.name);
                }
                for (auto &[node_index, rows] : rows_by_node) {
                    prepared.push_back(
                        {target_nodes[node_index],
                         RenderInsertSql(insert.table_name, insert.column_names,
                                         rows)});
                }
                if (!prepared.empty()) {
                    network::RequestEnvelope request{
                        .client_id = "__cluster_rebalance__",
                        .jwt_token = "",
                        .payload = "REBALANCE " + database_name + "." + table_name,
                    };
                    auto tx_result = ExecuteTwoPhaseCommit(prepared, request);
                    if (!tx_result.has_value() || tx_result->status_code != 200) {
                        return false;
                    }
                }
            }
        }
        return true;
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
    EntrypointServer::ExecutePreparedRequests(
        const std::vector<std::pair<StorageNodeEndpoint, std::string>> &prepared,
        const network::RequestEnvelope &request) const {
        for (const auto &[node, sql] : prepared) {
            network::RequestEnvelope forwarded = request;
            forwarded.payload = sql;
            auto response = ForwardToStorageNode(node, forwarded);
            if (!response.has_value() || response->status_code != 200) {
                return std::nullopt;
            }
        }
        return network::ResponseEnvelope{.status_code = 200,
                                         .payload = "sharded request applied"};
    }

    std::optional<network::ResponseEnvelope>
    EntrypointServer::ExecuteTwoPhaseCommit(
        const std::vector<StorageNodeEndpoint> &nodes,
        const network::RequestEnvelope &request) const {
        std::vector<std::pair<StorageNodeEndpoint, std::string>> prepared;
        prepared.reserve(nodes.size());
        for (const auto &node : nodes) {
            prepared.push_back({node, request.payload});
        }
        return ExecuteTwoPhaseCommit(prepared, request);
    }

    std::optional<network::ResponseEnvelope>
    EntrypointServer::ExecuteTwoPhaseCommit(
        const std::vector<std::pair<StorageNodeEndpoint, std::string>> &prepared,
        const network::RequestEnvelope &request) const {
        const std::string tx_id = common::UuidGenerator::NewGuidV4();

        std::vector<std::pair<StorageNodeEndpoint, std::string>> prepared_nodes;
        for (const auto &[node, sql] : prepared) {
            network::RequestEnvelope prepare_request = request;
            prepare_request.payload =
                "CLUSTER PREPARE_TX " + tx_id + " " + sql;
            auto prepare_response = ForwardToStorageNode(node, prepare_request);
            if (!prepare_response.has_value() || prepare_response->status_code != 200) {
                for (const auto &[prepared_node, prepared_sql] : prepared_nodes) {
                    (void)prepared_sql;
                    network::RequestEnvelope abort_request = request;
                    abort_request.payload = "CLUSTER ABORT_TX " + tx_id;
                    (void)ForwardToStorageNode(prepared_node, abort_request);
                }
                return std::nullopt;
            }
            prepared_nodes.push_back({node, sql});
        }

        for (const auto &[node, sql] : prepared_nodes) {
            (void)sql;
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
        const std::string endpoint_key =
            node.host + ":" + std::to_string(node.port);
        {
            std::scoped_lock lock(local_node_registry_mutex_);
            const auto it = local_node_registry_.find(endpoint_key);
            if (it != local_node_registry_.end() && it->second != nullptr) {
                return it->second->HandleRequest(request);
            }
        }
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
