#include "core/dbms_engine.hpp"

// this file wires parser, planner, and executor into one dbms request pipeline.
#include <algorithm>
namespace dbms::core {

    DbmsEngine::DbmsEngine(std::string root_path)
        : root_path_(std::move(root_path)), catalog_(root_path_),
          execution_(catalog_, runtime_state_, version_store_, string_pool_),
          persistence_(root_path_), wal_(root_path_) {
        persistence_.Load(runtime_state_, version_store_, string_pool_);
        if (!ReplayWal()) {
            wal_.QuarantineCorrupted();
        }
    }

    common::Result<execution::QueryResult>
    DbmsEngine::ExecuteSql(SessionContext &session, const std::string &sql) {
        return ExecuteSqlImpl(session, sql, true);
    }

    common::Result<execution::QueryResult>
    DbmsEngine::ExecuteSqlImpl(SessionContext &session, const std::string &sql,
                               bool write_wal) {
        auto parsed = parser_.Parse(sql);
        if (!parsed.ok()) {
            return {.value = std::nullopt, .error = parsed.error};
        }
        const auto &statement = *parsed.value;
        const auto transaction_key = TransactionKey(session);

        if (std::holds_alternative<parser::BeginTransactionStatement>(
                statement)) {
            if (transactions_.contains(transaction_key)) {
                return common::MakeError<execution::QueryResult>(
                    common::ErrorCode::kValidationError,
                    "transaction already active for session");
            }
            TransactionContext context;
            context.working_state = CloneRuntimeState(runtime_state_);
            context.working_version_store = version_store_;
            context.wal_statements.clear();
            transactions_.emplace(transaction_key, std::move(context));

            execution::QueryResult result;
            result.message = "transaction started";
            return common::MakeSuccess(std::move(result));
        }

        if (std::holds_alternative<parser::CommitTransactionStatement>(
                statement)) {
            auto transaction_it = transactions_.find(transaction_key);
            if (transaction_it == transactions_.end()) {
                return common::MakeError<execution::QueryResult>(
                    common::ErrorCode::kValidationError,
                    "no active transaction for session");
            }
            runtime_state_ = std::move(transaction_it->second.working_state);
            version_store_ =
                std::move(transaction_it->second.working_version_store);
            if (write_wal &&
                !transaction_it->second.wal_statements.empty() &&
                !wal_.AppendTransaction(transaction_it->second.wal_statements)) {
                return common::MakeError<execution::QueryResult>(
                    common::ErrorCode::kStorageError, "failed to append WAL TX");
            }
            transactions_.erase(transaction_it);
            if (!persistence_.Save(runtime_state_, version_store_, string_pool_)) {
                return common::MakeError<execution::QueryResult>(
                    common::ErrorCode::kStorageError, "failed to persist state");
            }
            if (write_wal && !wal_.Reset()) {
                return common::MakeError<execution::QueryResult>(
                    common::ErrorCode::kStorageError, "failed to checkpoint WAL");
            }

            execution::QueryResult result;
            result.message = "transaction committed";
            return common::MakeSuccess(std::move(result));
        }

        if (std::holds_alternative<parser::RollbackTransactionStatement>(
                statement)) {
            auto transaction_it = transactions_.find(transaction_key);
            if (transaction_it == transactions_.end()) {
                return common::MakeError<execution::QueryResult>(
                    common::ErrorCode::kValidationError,
                    "no active transaction for session");
            }
            transactions_.erase(transaction_it);
            execution::QueryResult result;
            result.message = "transaction rolled back";
            return common::MakeSuccess(std::move(result));
        }

        auto transaction_it = transactions_.find(transaction_key);
        if (transaction_it != transactions_.end()) {
            if (std::holds_alternative<parser::CheckIndexStatement>(statement) ||
                std::holds_alternative<parser::RebuildIndexStatement>(statement)) {
                return common::MakeError<execution::QueryResult>(
                    common::ErrorCode::kValidationError,
                    "service index commands are not allowed inside active transaction");
            }
            if (IsDdlStatement(statement)) {
                return common::MakeError<execution::QueryResult>(
                    common::ErrorCode::kValidationError,
                    "DDL statements are not allowed inside an active transaction");
            }
            planner::Planner transactional_planner(&transaction_it->second.working_state);
            auto plan = transactional_planner.BuildPlan(std::move(*parsed.value));
            if (!plan.ok()) {
                return {.value = std::nullopt, .error = plan.error};
            }
            execution::ExecutionEngine transactional_execution(
                catalog_, transaction_it->second.working_state,
                transaction_it->second.working_version_store, string_pool_);
            auto tx_result = transactional_execution.Execute(session, *plan.value);
            if (tx_result.ok() && write_wal &&
                IsMutatingStatement(*plan.value->statement)) {
                transaction_it->second.wal_statements.push_back(sql);
            }
            return tx_result;
        }

        auto plan = planner_.BuildPlan(std::move(*parsed.value));
        if (!plan.ok()) {
            return {.value = std::nullopt, .error = plan.error};
        }

        auto execution_result = execution_.Execute(session, *plan.value);
        if (!execution_result.ok()) {
            return execution_result;
        }

        if (IsMutatingStatement(*plan.value->statement)) {
            if (write_wal && !wal_.AppendSql(sql)) {
                return common::MakeError<execution::QueryResult>(
                    common::ErrorCode::kStorageError, "failed to append WAL SQL");
            }
            if (!persistence_.Save(runtime_state_, version_store_, string_pool_)) {
                return common::MakeError<execution::QueryResult>(
                    common::ErrorCode::kStorageError, "failed to persist state");
            }
            if (write_wal && !wal_.Reset()) {
                return common::MakeError<execution::QueryResult>(
                    common::ErrorCode::kStorageError, "failed to checkpoint WAL");
            }
        }
        return execution_result;
    }

    RuntimeState &DbmsEngine::runtime_state() { return runtime_state_; }

    versioning::VersionStore &DbmsEngine::version_store() {
        return version_store_;
    }

    storage::StringPool &DbmsEngine::string_pool() { return string_pool_; }

    catalog::Catalog &DbmsEngine::catalog() { return catalog_; }

    bool DbmsEngine::IsMutatingStatement(
        const parser::Statement &statement) const {
        if (std::holds_alternative<parser::SelectStatement>(statement) ||
            std::holds_alternative<parser::UseDatabaseStatement>(statement) ||
            std::holds_alternative<parser::CheckIndexStatement>(statement)) {
            return false;
        }
        return true;
    }

    bool DbmsEngine::IsDdlStatement(
        const parser::Statement &statement) const {
        return std::holds_alternative<parser::CreateDatabaseStatement>(statement) ||
               std::holds_alternative<parser::DropDatabaseStatement>(statement) ||
               std::holds_alternative<parser::CreateTableStatement>(statement) ||
               std::holds_alternative<parser::DropTableStatement>(statement);
    }

    std::string DbmsEngine::TransactionKey(
        const SessionContext &session) const {
        if (!session.client_id.empty()) {
            return session.client_id;
        }
        return "__default_session__";
    }

    bool DbmsEngine::ReplayWal() {
        std::vector<WalEntry> entries;
        if (!wal_.LoadAll(entries)) {
            return false;
        }
        if (entries.empty()) {
            wal_.Reset();
            return true;
        }

        SessionContext recovery_session;
        recovery_session.client_id = "__wal_recovery__";
        for (const auto &entry : entries) {
            if (entry.type == WalEntry::Type::kSql) {
                if (entry.statements.empty()) {
                    continue;
                }
                auto result =
                    ExecuteSqlImpl(recovery_session, entry.statements.front(), false);
                if (!result.ok()) {
                    return false;
                }
                continue;
            }
            auto begin = ExecuteSqlImpl(recovery_session, "BEGIN;", false);
            if (!begin.ok()) {
                return false;
            }
            for (const auto &statement : entry.statements) {
                auto tx_result = ExecuteSqlImpl(recovery_session, statement, false);
                if (!tx_result.ok()) {
                    ExecuteSqlImpl(recovery_session, "ROLLBACK;", false);
                    return false;
                }
            }
            auto commit = ExecuteSqlImpl(recovery_session, "COMMIT;", false);
            if (!commit.ok()) {
                return false;
            }
        }
        wal_.Reset();
        return true;
    }

} // namespace dbms::core
