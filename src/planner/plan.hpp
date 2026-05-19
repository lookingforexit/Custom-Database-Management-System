#pragma once

// this file defines physical plan node shapes produced by the planner.
#include <string>
#include <vector>

namespace dbms::planner {

    enum class PlanNodeKind {
        kCreateDatabase,
        kDropDatabase,
        kUseDatabase,
        kCreateTable,
        kDropTable,
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

} // namespace dbms::planner
