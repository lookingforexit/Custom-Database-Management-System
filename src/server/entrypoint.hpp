#pragma once

// this file defines the entrypoint facade that manages requests and client
// sessions.
#include <cstdint>
#include <string>
#include <optional>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <unordered_set>
#include <atomic>
#include <utility>

#include "core/dbms_engine.hpp"
#include "catalog/rbac.hpp"
#include "catalog/schema.hpp"
#include "common/types.hpp"
#include "network/protocol.hpp"
#include "parser/parser.hpp"
#include "runtime/job_queue.hpp"
#include "runtime/telemetry.hpp"

namespace dbms::server {

    class EntrypointServer {
      public:
        explicit EntrypointServer(std::string root_path);

        [[nodiscard]] network::ResponseEnvelope
        HandleRequest(const network::RequestEnvelope &request);
        [[nodiscard]] bool RunHeartbeatCycle();

      private:
        struct StorageNodeEndpoint {
            std::string host;
            int port{0};
            bool managed{false};
            int fail_count{0};
        };
        struct PendingClusterTransaction {
            std::string sql;
            std::string client_id;
            std::string current_database;
        };

        bool RestartManagedNode(StorageNodeEndpoint &node) const;

        core::SessionContext &GetOrCreateSession(const std::string &client_id);
        [[nodiscard]] bool ParseClusterCommand(const network::RequestEnvelope &request,
                                               network::ResponseEnvelope &response);
        [[nodiscard]] bool ParseAsyncCommand(const network::RequestEnvelope &request,
                                             network::ResponseEnvelope &response);
        [[nodiscard]] bool ParseTelemetryCommand(const std::string &payload,
                                                 network::ResponseEnvelope &response);
        [[nodiscard]] std::string
        SerializeLocalTelemetry(const runtime::TelemetrySnapshot &snapshot) const;
        [[nodiscard]] std::optional<runtime::TelemetrySnapshot>
        ParseLocalTelemetry(const std::string &payload) const;
        [[nodiscard]] runtime::TelemetrySnapshot
        AggregateTelemetry(
            const runtime::TelemetrySnapshot &local_snapshot,
            const std::vector<runtime::TelemetrySnapshot> &remote_snapshots) const;
        [[nodiscard]] bool ParseAuthCommand(const network::RequestEnvelope &request,
                                            network::ResponseEnvelope &response);
        [[nodiscard]] std::optional<catalog::Permission>
        RequiredPermission(const parser::Statement &statement) const;
        [[nodiscard]] std::string
        ResolveAuthorizationDatabaseName(const parser::Statement &statement,
                                         const core::SessionContext &session) const;
        struct ShardRoutingDecision {
            std::size_t node_index{0};
            bool by_shard_key{false};
        };
        struct DumpedTableRow {
            std::vector<common::Value> values;
        };
        [[nodiscard]] std::optional<catalog::TableSchema>
        ResolveClusterTableSchema(const parser::QualifiedName &table_name,
                                  const core::SessionContext &session) const;
        [[nodiscard]] std::optional<std::string>
        ResolveClusterDatabaseName(const parser::QualifiedName &table_name,
                                   const core::SessionContext &session) const;
        [[nodiscard]] std::optional<std::string>
        ResolveShardKeyColumn(const catalog::TableSchema &schema) const;
        [[nodiscard]] std::optional<common::Value>
        TryExtractShardKeyValue(const parser::Expression &expr,
                                const std::string &shard_key_column) const;
        [[nodiscard]] std::optional<std::size_t>
        RouteNodeIndex(const parser::Statement &statement,
                       const std::vector<StorageNodeEndpoint> &nodes,
                       const core::SessionContext &session) const;
        [[nodiscard]] ShardRoutingDecision
        RouteValueToNode(const common::Value &value,
                         const std::vector<StorageNodeEndpoint> &nodes) const;
        [[nodiscard]] std::optional<std::string>
        ExtractTargetTableName(const parser::Statement &statement) const;
        [[nodiscard]] bool ShouldBroadcast(const parser::Statement &statement) const;
        [[nodiscard]] bool IsSelectStatement(const parser::Statement &statement) const;
        [[nodiscard]] bool IsMutatingStatement(const parser::Statement &statement) const;
        [[nodiscard]] bool IsMetadataStatement(const parser::Statement &statement) const;
        [[nodiscard]] std::string RenderLiteralSql(const common::Value &value) const;
        [[nodiscard]] std::string RenderInsertSql(
            const parser::QualifiedName &table_name,
            const std::vector<std::string> &column_names,
            const std::vector<std::vector<common::Value>> &rows) const;
        [[nodiscard]] std::string
        RenderCreateTableSql(const catalog::TableSchema &schema) const;
        [[nodiscard]] std::optional<std::unordered_map<std::size_t, std::string>>
        SplitInsertByShard(const parser::InsertStatement &insert,
                           const std::vector<StorageNodeEndpoint> &nodes,
                           const core::SessionContext &session) const;
        [[nodiscard]] std::optional<std::vector<DumpedTableRow>>
        FetchTableDump(const StorageNodeEndpoint &node,
                       const std::string &database_name,
                       const std::string &table_name) const;
        [[nodiscard]] bool
        RebalanceClusterData(const std::vector<StorageNodeEndpoint> &source_nodes,
                             const std::vector<StorageNodeEndpoint> &target_nodes,
                             bool truncate_targets) const;
        [[nodiscard]] bool
        SyncMetadataToNodes(
            const std::vector<StorageNodeEndpoint> &target_nodes) const;
        [[nodiscard]] std::optional<network::ResponseEnvelope>
        ExecutePreparedRequests(
            const std::vector<std::pair<StorageNodeEndpoint, std::string>> &prepared,
            const network::RequestEnvelope &request) const;
        [[nodiscard]] std::string
        QueryResultToJson(const execution::QueryResult &result) const;
        [[nodiscard]] std::optional<network::ResponseEnvelope>
        ExecuteTwoPhaseCommit(const std::vector<StorageNodeEndpoint> &nodes,
                              const network::RequestEnvelope &request) const;
        [[nodiscard]] std::optional<network::ResponseEnvelope>
        ExecuteTwoPhaseCommit(
            const std::vector<std::pair<StorageNodeEndpoint, std::string>> &prepared,
            const network::RequestEnvelope &request) const;
        [[nodiscard]] std::optional<network::ResponseEnvelope>
        BroadcastToStorageNodes(const std::vector<StorageNodeEndpoint> &nodes,
                                const network::RequestEnvelope &request,
                                bool collect_json,
                                std::string &merged_json) const;
        [[nodiscard]] std::optional<network::ResponseEnvelope>
        ForwardToStorageNode(const StorageNodeEndpoint &node,
                             const network::RequestEnvelope &request) const;
        void WriteAccessLog(
            const network::RequestEnvelope &request,
            const network::ResponseEnvelope &response,
            const std::string &handler_id,
            std::chrono::system_clock::time_point started_at_utc,
            std::chrono::system_clock::time_point finished_at_utc,
            std::int64_t latency_ms) const;

        core::DbmsEngine engine_;
        parser::Parser parser_;
        runtime::JobQueue job_queue_;
        runtime::TelemetryRegistry telemetry_;
        catalog::AccessController access_controller_;
        std::unordered_map<std::string, core::SessionContext> sessions_;
        std::vector<StorageNodeEndpoint> storage_nodes_;
        mutable std::mutex nodes_mutex_;
        mutable std::mutex pending_transactions_mutex_;
        std::unordered_map<std::string, PendingClusterTransaction>
            pending_cluster_transactions_;
        std::string access_log_path_;
        mutable std::mutex access_log_mutex_;
        mutable std::atomic<std::uint64_t> next_handler_id_{1};
    };

} // namespace dbms::server
