#include "execution/execution_engine.hpp"

// this file implements execution entry points for scans, writes, and
// aggregates.
namespace dbms::execution {

  ExecutionEngine::ExecutionEngine(catalog::Catalog &catalog,
                                   core::RuntimeState &runtime_state,
                                   versioning::VersionStore &version_store,
                                   storage::StringPool &string_pool)
      : catalog_(catalog), runtime_state_(runtime_state),
        version_store_(version_store), string_pool_(string_pool) {}

  common::Result<QueryResult>
  ExecutionEngine::Execute(core::SessionContext &session,
                           const planner::PlanNode &) {
    (void)session;
    QueryResult result;
    result.message = "Execution stub";
    return {.value = std::move(result), .error = std::nullopt};
  }

} // namespace dbms::execution
