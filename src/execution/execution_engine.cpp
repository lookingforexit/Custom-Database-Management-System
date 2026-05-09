#include "execution/execution_engine.hpp"

// this file will execute scans, filters, writes, and aggregations.
namespace dbms::execution {

common::Result<QueryResult> ExecutionEngine::Execute(const planner::PlanNode&) const {
    QueryResult result;
    result.message = "Execution stub";
    return {.value = std::move(result), .error = std::nullopt};
}

}  // namespace dbms::execution
