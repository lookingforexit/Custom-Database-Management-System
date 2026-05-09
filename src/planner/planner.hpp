#pragma once

// this file declares the planner that picks scans and execution steps.
#include "common/result.hpp"
#include "parser/ast.hpp"
#include "planner/plan.hpp"

namespace dbms::planner {

class Planner {
public:
    [[nodiscard]] common::Result<PlanNode> BuildPlan(
        const parser::Statement& statement) const;
};

}  // namespace dbms::planner
