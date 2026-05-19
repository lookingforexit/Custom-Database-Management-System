#include <cassert>
#include <string>

#include "parser/parser.hpp"
#include "planner/planner.hpp"
#include "core/runtime_state.hpp"
#include "storage/table_heap.hpp"

namespace {

    dbms::planner::PlanNode BuildPlanOrAbort(dbms::planner::Planner &planner,
                                             const std::string &sql) {
        dbms::parser::Parser parser;
        auto parsed = parser.Parse(sql);
        assert(parsed.ok());

        auto plan = planner.BuildPlan(std::move(*parsed.value));
        assert(plan.ok());
        return std::move(*plan.value);
    }

} // namespace

int main() {
    using dbms::planner::PlanNodeKind;
    dbms::core::RuntimeState runtime_state;
    dbms::planner::Planner planner(&runtime_state);

    // indexed table: planner should choose IndexScan for supported predicates.
    {
        dbms::core::DatabaseRuntime database_runtime;
        database_runtime.name = "db1";
        dbms::core::TableRuntime table_runtime;
        table_runtime.schema.database_name = "db1";
        table_runtime.schema.table_name = "t";
        table_runtime.schema.columns.push_back(
            {.name = "id",
             .type = dbms::common::ValueType::kInt64,
             .constraint = dbms::catalog::ColumnConstraint::kIndexed,
             .default_value = std::nullopt});
        table_runtime.heap =
            std::make_unique<dbms::storage::TableHeap>(table_runtime.schema);
        table_runtime.indexes.emplace(
            "id", std::make_unique<dbms::index::BStarPlusTree>());
        database_runtime.tables.emplace("t", std::move(table_runtime));
        runtime_state.databases.emplace("db1", std::move(database_runtime));
    }

    {
        auto plan = BuildPlanOrAbort(planner, "CREATE DATABASE db1;");
        assert(plan.kind == PlanNodeKind::kCreateDatabase);
    }
    {
        auto plan = BuildPlanOrAbort(planner, "DROP DATABASE db1;");
        assert(plan.kind == PlanNodeKind::kDropDatabase);
    }
    {
        auto plan = BuildPlanOrAbort(planner, "USE db1;");
        assert(plan.kind == PlanNodeKind::kUseDatabase);
    }
    {
        auto plan = BuildPlanOrAbort(planner, "CREATE TABLE db1.t (id INT INDEXED);");
        assert(plan.kind == PlanNodeKind::kCreateTable);
    }
    {
        auto plan = BuildPlanOrAbort(planner, "DROP TABLE db1.t;");
        assert(plan.kind == PlanNodeKind::kDropTable);
    }
    {
        auto plan = BuildPlanOrAbort(planner, "INSERT INTO db1.t (id) VALUES (1), (2);");
        assert(plan.kind == PlanNodeKind::kInsert);
    }
    {
        auto plan = BuildPlanOrAbort(planner, "UPDATE db1.t SET id = 3 WHERE id == 1;");
        assert(plan.kind == PlanNodeKind::kUpdate);
    }
    {
        auto plan = BuildPlanOrAbort(planner, "DELETE FROM db1.t WHERE id == 1;");
        assert(plan.kind == PlanNodeKind::kDelete);
    }
    {
        auto plan = BuildPlanOrAbort(planner, "SELECT id FROM db1.t;");
        assert(plan.kind == PlanNodeKind::kProject);
        assert(plan.children.size() == 1);
        assert(plan.children[0].kind == PlanNodeKind::kSeqScan);
    }
    {
        auto plan = BuildPlanOrAbort(planner, "SELECT id FROM db1.t WHERE id == 1;");
        assert(plan.kind == PlanNodeKind::kProject);
        assert(plan.children.size() == 1);
        assert(plan.children[0].kind == PlanNodeKind::kFilter);
        assert(plan.children[0].children.size() == 1);
        assert(plan.children[0].children[0].kind == PlanNodeKind::kIndexScan);
    }
    {
        auto plan = BuildPlanOrAbort(planner, "SELECT id FROM db1.t WHERE id BETWEEN 1 AND 10;");
        assert(plan.kind == PlanNodeKind::kProject);
        assert(plan.children.size() == 1);
        assert(plan.children[0].kind == PlanNodeKind::kFilter);
        assert(plan.children[0].children.size() == 1);
        assert(plan.children[0].children[0].kind == PlanNodeKind::kIndexScan);
    }
    {
        // no index on name => SeqScan fallback
        auto plan =
            BuildPlanOrAbort(planner, "SELECT id FROM db1.t WHERE name == \"A\";");
        assert(plan.kind == PlanNodeKind::kProject);
        assert(plan.children.size() == 1);
        assert(plan.children[0].kind == PlanNodeKind::kFilter);
        assert(plan.children[0].children.size() == 1);
        assert(plan.children[0].children[0].kind == PlanNodeKind::kSeqScan);
    }
    {
        auto plan = BuildPlanOrAbort(planner, "SELECT COUNT(id) FROM db1.t WHERE id >= 1;");
        assert(plan.kind == PlanNodeKind::kAggregate);
        assert(plan.children.size() == 1);
        assert(plan.children[0].kind == PlanNodeKind::kProject);
    }
    {
        auto plan =
            BuildPlanOrAbort(planner, "REVERT db1.t \"2026.05.19-10:00:00.000001\";");
        assert(plan.kind == PlanNodeKind::kRevert);
    }
    {
        auto plan = BuildPlanOrAbort(planner, "BEGIN;");
        assert(plan.kind == PlanNodeKind::kBeginTransaction);
    }
    {
        auto plan = BuildPlanOrAbort(planner, "COMMIT;");
        assert(plan.kind == PlanNodeKind::kCommitTransaction);
    }
    {
        auto plan = BuildPlanOrAbort(planner, "ROLLBACK;");
        assert(plan.kind == PlanNodeKind::kRollbackTransaction);
    }

    return 0;
}
