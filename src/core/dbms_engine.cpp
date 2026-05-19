#include "core/dbms_engine.hpp"

// this file wires parser, planner, and executor into one dbms request pipeline.
namespace dbms::core {

    DbmsEngine::DbmsEngine(std::string root_path)
        : root_path_(std::move(root_path)), catalog_(root_path_),
          execution_(catalog_, runtime_state_, version_store_, string_pool_),
          persistence_(root_path_) {
        persistence_.Load(runtime_state_);
    }

    common::Result<execution::QueryResult>
    DbmsEngine::ExecuteSql(SessionContext &session, const std::string &sql) {
        auto parsed = parser_.Parse(sql);
        if (!parsed.ok()) {
            return {.value = std::nullopt, .error = parsed.error};
        }

        auto plan = planner_.BuildPlan(std::move(*parsed.value));
        if (!plan.ok()) {
            return {.value = std::nullopt, .error = plan.error};
        }

        auto execution_result = execution_.Execute(session, *plan.value);
        if (!execution_result.ok()) {
            return execution_result;
        }

        const auto &statement = *plan.value->statement;
        if (!std::holds_alternative<parser::SelectStatement>(statement) &&
            !std::holds_alternative<parser::UseDatabaseStatement>(statement)) {
            persistence_.Save(runtime_state_);
        }
        return execution_result;
    }

    RuntimeState &DbmsEngine::runtime_state() { return runtime_state_; }

    catalog::Catalog &DbmsEngine::catalog() { return catalog_; }

} // namespace dbms::core
