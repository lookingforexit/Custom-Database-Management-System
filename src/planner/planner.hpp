#pragma once

// this file defines the planner interface that maps ast statements to plans.
#include "common/result.hpp"
#include "parser/ast.hpp"
#include "planner/plan.hpp"

namespace dbms::planner {

    class Planner {
      public:
        [[nodiscard]] common::Result<PlanNode>
        BuildPlan(parser::Statement statement) const;
    };

} // namespace dbms::planner
