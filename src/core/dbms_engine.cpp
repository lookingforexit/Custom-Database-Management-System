#include "core/dbms_engine.hpp"

// this file wires parser, planner, and executor into one dbms request pipeline.
namespace dbms::core {

    DbmsEngine::DbmsEngine(std::string root_path)
        : root_path_(std::move(root_path)), catalog_(root_path_),
          execution_(catalog_, runtime_state_, version_store_, string_pool_),
          persistence_(root_path_) {
        persistence_.Load(runtime_state_, version_store_);
    }

    common::Result<execution::QueryResult>
    DbmsEngine::ExecuteSql(SessionContext &session, const std::string &sql) {
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
            transactions_.erase(transaction_it);
            persistence_.Save(runtime_state_, version_store_);

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
            return transactional_execution.Execute(session, *plan.value);
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
            persistence_.Save(runtime_state_, version_store_);
        }
        return execution_result;
    }

    RuntimeState &DbmsEngine::runtime_state() { return runtime_state_; }

    versioning::VersionStore &DbmsEngine::version_store() {
        return version_store_;
    }

    catalog::Catalog &DbmsEngine::catalog() { return catalog_; }

    bool DbmsEngine::IsMutatingStatement(
        const parser::Statement &statement) const {
        if (std::holds_alternative<parser::SelectStatement>(statement) ||
            std::holds_alternative<parser::UseDatabaseStatement>(statement)) {
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

} // namespace dbms::core
