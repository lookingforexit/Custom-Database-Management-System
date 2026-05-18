#include "planner/planner.hpp"

// this file implements physical plan construction from parsed sql statements.
namespace dbms::planner {

  common::Result<PlanNode> Planner::BuildPlan(const parser::Statement &) const {
    PlanNode node;
    return {.value = std::move(node), .error = std::nullopt};
  }

} // namespace dbms::planner
