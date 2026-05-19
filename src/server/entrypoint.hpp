#pragma once

// this file defines the entrypoint facade that manages requests and client
// sessions.
#include <string>
#include <optional>
#include <vector>
#include <unordered_map>
#include <mutex>

#include "core/dbms_engine.hpp"
#include "network/protocol.hpp"
#include "parser/parser.hpp"

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

        bool RestartManagedNode(StorageNodeEndpoint &node) const;

        core::SessionContext &GetOrCreateSession(const std::string &client_id);
        [[nodiscard]] bool ParseClusterCommand(const std::string &payload,
                                               network::ResponseEnvelope &response);
        [[nodiscard]] std::optional<std::size_t>
        RouteNodeIndex(const parser::Statement &statement,
                       const std::vector<StorageNodeEndpoint> &nodes) const;
        [[nodiscard]] std::optional<std::string>
        ExtractTargetTableName(const parser::Statement &statement) const;
        [[nodiscard]] bool ShouldBroadcast(const parser::Statement &statement) const;
        [[nodiscard]] bool IsSelectStatement(const parser::Statement &statement) const;
        [[nodiscard]] std::string
        QueryResultToJson(const execution::QueryResult &result) const;
        [[nodiscard]] std::optional<network::ResponseEnvelope>
        BroadcastToStorageNodes(const std::vector<StorageNodeEndpoint> &nodes,
                                const network::RequestEnvelope &request,
                                bool collect_json,
                                std::string &merged_json) const;
        [[nodiscard]] std::optional<network::ResponseEnvelope>
        ForwardToStorageNode(const StorageNodeEndpoint &node,
                             const network::RequestEnvelope &request) const;

        core::DbmsEngine engine_;
        parser::Parser parser_;
        std::unordered_map<std::string, core::SessionContext> sessions_;
        std::vector<StorageNodeEndpoint> storage_nodes_;
        mutable std::mutex nodes_mutex_;
    };

} // namespace dbms::server
