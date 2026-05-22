#pragma once

// this file defines the top-level dbms facade that orchestrates sql execution.

#include "catalog/catalog.hpp"
#include "common/result.hpp"
#include "execution/execution_engine.hpp"
#include "parser/parser.hpp"
#include "planner/planner.hpp"
#include "runtime_persistence.hpp"
#include "runtime_state.hpp"
#include "session_context.hpp"
#include "storage/string_pool.hpp"
#include "versioning/version_store.hpp"
#include "wal_manager.hpp"
#include <unordered_map>
#include <vector>

namespace dbms::core {

    class DbmsEngine {
      public:
        explicit DbmsEngine(std::string root_path);

        [[nodiscard]] RuntimeState &runtime_state();
        [[nodiscard]] const RuntimeState &runtime_state() const;
        [[nodiscard]] versioning::VersionStore &version_store();
        [[nodiscard]] storage::StringPool &string_pool();
        [[nodiscard]] catalog::Catalog &catalog();

        [[nodiscard]] common::Result<execution::QueryResult>
        ExecuteSql(SessionContext &session, const std::string &sql);

      private:
        struct TransactionContext {
            RuntimeState working_state;
            versioning::VersionStore working_version_store;
            std::vector<std::string> wal_statements;
        };

        [[nodiscard]] bool IsMutatingStatement(
            const parser::Statement &statement) const;
        [[nodiscard]] bool IsDdlStatement(
            const parser::Statement &statement) const;
        [[nodiscard]] std::string TransactionKey(
            const SessionContext &session) const;
        common::Result<execution::QueryResult>
        ExecuteSqlImpl(SessionContext &session, const std::string &sql,
                       bool write_wal);
        bool ReplayWal();

        std::string root_path_;
        RuntimeState runtime_state_;
        std::unordered_map<std::string, TransactionContext> transactions_;

        parser::Parser parser_;
        planner::Planner planner_{&runtime_state_};
        catalog::Catalog catalog_;
        versioning::VersionStore version_store_;
        storage::StringPool string_pool_;
        execution::ExecutionEngine execution_;
        RuntimePersistence persistence_;
        WalManager wal_;
    };

} // namespace dbms::core
