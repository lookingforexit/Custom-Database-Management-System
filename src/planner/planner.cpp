#include "planner/planner.hpp"

// this file will map ast statements to optimized physical plans.
namespace dbms::planner {

common::Result<PlanNode> Planner::BuildPlan(const parser::Statement&) const {
    PlanNode node;
    return {.value = std::move(node), .error = std::nullopt};
}

}  // namespace dbms::planner
