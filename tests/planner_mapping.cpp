#include <cassert>
#include <string>

#include "parser/parser.hpp"
#include "planner/planner.hpp"

namespace {

    dbms::planner::PlanNode BuildPlanOrAbort(const std::string &sql) {
        dbms::parser::Parser parser;
        const auto parsed = parser.Parse(sql);
        assert(parsed.ok());

        dbms::planner::Planner planner;
        auto plan = planner.BuildPlan(*parsed.value);
        assert(plan.ok());
        return std::move(*plan.value);
    }

} // namespace

int main() {
    using dbms::planner::PlanNodeKind;

    {
        auto plan = BuildPlanOrAbort("CREATE DATABASE db1;");
        assert(plan.kind == PlanNodeKind::kCreateDatabase);
    }
    {
        auto plan = BuildPlanOrAbort("DROP DATABASE db1;");
        assert(plan.kind == PlanNodeKind::kDropDatabase);
    }
    {
        auto plan = BuildPlanOrAbort("USE db1;");
        assert(plan.kind == PlanNodeKind::kUseDatabase);
    }
    {
        auto plan = BuildPlanOrAbort("CREATE TABLE db1.t (id INT INDEXED);");
        assert(plan.kind == PlanNodeKind::kCreateTable);
    }
    {
        auto plan = BuildPlanOrAbort("DROP TABLE db1.t;");
        assert(plan.kind == PlanNodeKind::kDropTable);
    }
    {
        auto plan = BuildPlanOrAbort("INSERT INTO db1.t (id) VALUES (1), (2);");
        assert(plan.kind == PlanNodeKind::kInsert);
    }
    {
        auto plan = BuildPlanOrAbort("UPDATE db1.t SET id = 3 WHERE id == 1;");
        assert(plan.kind == PlanNodeKind::kUpdate);
    }
    {
        auto plan = BuildPlanOrAbort("DELETE FROM db1.t WHERE id == 1;");
        assert(plan.kind == PlanNodeKind::kDelete);
    }
    {
        auto plan = BuildPlanOrAbort("SELECT id FROM db1.t;");
        assert(plan.kind == PlanNodeKind::kProject);
        assert(plan.children.size() == 1);
        assert(plan.children[0].kind == PlanNodeKind::kSeqScan);
    }
    {
        auto plan = BuildPlanOrAbort("SELECT id FROM db1.t WHERE id == 1;");
        assert(plan.kind == PlanNodeKind::kProject);
        assert(plan.children.size() == 1);
        assert(plan.children[0].kind == PlanNodeKind::kFilter);
        assert(plan.children[0].children.size() == 1);
        assert(plan.children[0].children[0].kind == PlanNodeKind::kSeqScan);
    }
    {
        auto plan = BuildPlanOrAbort("SELECT COUNT(id) FROM db1.t WHERE id >= 1;");
        assert(plan.kind == PlanNodeKind::kAggregate);
        assert(plan.children.size() == 1);
        assert(plan.children[0].kind == PlanNodeKind::kProject);
    }
    {
        auto plan =
            BuildPlanOrAbort("REVERT db1.t \"2026.05.19-10:00:00.000001\";");
        assert(plan.kind == PlanNodeKind::kRevert);
    }

    return 0;
}
