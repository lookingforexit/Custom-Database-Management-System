#pragma once

// this file defines physical plan node shapes for execution.
#include <string>
#include <vector>

namespace dbms::planner {

enum class PlanNodeKind {
    kSeqScan,
    kIndexScan,
    kFilter,
    kProject,
    kAggregate,
    kInsert,
    kUpdate,
    kDelete,
    kRevert,
    kRemoteShardDispatch,
};

struct PlanNode {
    PlanNodeKind kind{PlanNodeKind::kSeqScan};
    std::string detail;
    std::vector<PlanNode> children;
};

}  // namespace dbms::planner
