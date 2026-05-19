#pragma once

// this file defines physical plan node shapes produced by the planner.
#include <memory>
#include <string>
#include <vector>

#include "parser/ast.hpp"

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
        std::shared_ptr<const parser::Statement> statement;
        std::vector<PlanNode> children;
    };

} // namespace dbms::planner
