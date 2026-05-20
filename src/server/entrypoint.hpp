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

#include "core/dbms_engine.hpp"
#include "catalog/rbac.hpp"
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
        };

        bool RestartManagedNode(StorageNodeEndpoint &node) const;

        core::SessionContext &GetOrCreateSession(const std::string &client_id);
        [[nodiscard]] bool ParseClusterCommand(const std::string &payload,
                                               network::ResponseEnvelope &response);
        [[nodiscard]] bool ParseAsyncCommand(const network::RequestEnvelope &request,
                                             network::ResponseEnvelope &response);
        [[nodiscard]] bool ParseTelemetryCommand(const std::string &payload,
                                                 network::ResponseEnvelope &response);
        [[nodiscard]] bool ParseAuthCommand(const network::RequestEnvelope &request,
                                            network::ResponseEnvelope &response);
        [[nodiscard]] std::optional<catalog::Permission>
        RequiredPermission(const parser::Statement &statement) const;
        [[nodiscard]] std::optional<std::size_t>
        RouteNodeIndex(const parser::Statement &statement,
                       const std::vector<StorageNodeEndpoint> &nodes) const;
        [[nodiscard]] std::optional<std::string>
        ExtractTargetTableName(const parser::Statement &statement) const;
        [[nodiscard]] bool ShouldBroadcast(const parser::Statement &statement) const;
        [[nodiscard]] bool IsSelectStatement(const parser::Statement &statement) const;
        [[nodiscard]] bool IsMutatingStatement(const parser::Statement &statement) const;
        [[nodiscard]] std::string
        QueryResultToJson(const execution::QueryResult &result) const;
        [[nodiscard]] std::optional<network::ResponseEnvelope>
        ExecuteTwoPhaseCommit(const std::vector<StorageNodeEndpoint> &nodes,
                              const network::RequestEnvelope &request) const;
        [[nodiscard]] std::optional<network::ResponseEnvelope>
        BroadcastToStorageNodes(const std::vector<StorageNodeEndpoint> &nodes,
                                const network::RequestEnvelope &request,
                                bool collect_json,
                                std::string &merged_json) const;
        [[nodiscard]] std::optional<network::ResponseEnvelope>
        ForwardToStorageNode(const StorageNodeEndpoint &node,
                             const network::RequestEnvelope &request) const;
        void WriteAccessLog(const network::RequestEnvelope &request,
                            const network::ResponseEnvelope &response,
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
    };

} // namespace dbms::server
