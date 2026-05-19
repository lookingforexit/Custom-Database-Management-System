#pragma once

// this file defines the planner interface that maps ast statements to plans.
#include "common/result.hpp"
#include "core/runtime_state.hpp"
#include "parser/ast.hpp"
#include "planner/plan.hpp"

namespace dbms::planner {

    class Planner {
      public:
        explicit Planner(core::RuntimeState *runtime_state = nullptr);

        [[nodiscard]] common::Result<PlanNode>
        BuildPlan(parser::Statement statement) const;

        void set_runtime_state(core::RuntimeState *runtime_state);

      private:
        core::RuntimeState *runtime_state_{nullptr};
    };

} // namespace dbms::planner
