#pragma once

// this file declares the executor that runs physical plan operators.
#include <string>
#include <vector>

#include "common/result.hpp"
#include "common/types.hpp"
#include "planner/plan.hpp"

namespace dbms::execution {

struct QueryResult {
    std::vector<common::RowData> rows;
    std::string message;
};

class ExecutionEngine {
public:
    [[nodiscard]] common::Result<QueryResult> Execute(
        const planner::PlanNode& plan) const;
};

}  // namespace dbms::execution
