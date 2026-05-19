#include "planner/planner.hpp"

#include <sstream>

#include "common/result.hpp"

// this file implements physical plan construction from parsed sql statements.
namespace dbms::planner {

    namespace {

        std::string QualifiedNameToString(const parser::QualifiedName &name) {
            if (name.database_name.has_value()) {
                return *name.database_name + "." + name.object_name;
            }
            return name.object_name;
        }

        std::string SelectItemsSummary(
            const std::vector<parser::SelectItem> &items) {
            if (items.size() == 1 && items[0].is_wildcard) {
                return "*";
            }

            std::ostringstream out;
            for (std::size_t i = 0; i < items.size(); ++i) {
                const auto &item = items[i];
                if (i != 0) {
                    out << ", ";
                }
                if (item.aggregate.has_value()) {
                    switch (*item.aggregate) {
                        case parser::AggregateKind::kSum:
                            out << "SUM(" << item.column_name << ")";
                            break;
                        case parser::AggregateKind::kCount:
                            out << "COUNT(" << item.column_name << ")";
                            break;
                        case parser::AggregateKind::kAvg:
                            out << "AVG(" << item.column_name << ")";
                            break;
                    }
                } else {
                    out << item.column_name;
                }
                if (item.alias.has_value()) {
                    out << " AS " << *item.alias;
                }
            }
            return out.str();
        }

    } // namespace

    common::Result<PlanNode>
    Planner::BuildPlan(const parser::Statement &statement) const {
        PlanNode node;

        if (const auto *create_db =
                std::get_if<parser::CreateDatabaseStatement>(&statement);
            create_db != nullptr) {
            node.kind = PlanNodeKind::kCreateDatabase;
            node.detail = "create database " + create_db->database_name;
            return common::MakeSuccess(std::move(node));
        }

        if (const auto *drop_db =
                std::get_if<parser::DropDatabaseStatement>(&statement);
            drop_db != nullptr) {
            node.kind = PlanNodeKind::kDropDatabase;
            node.detail = "drop database " + drop_db->database_name;
            return common::MakeSuccess(std::move(node));
        }

        if (const auto *use_db =
                std::get_if<parser::UseDatabaseStatement>(&statement);
            use_db != nullptr) {
            node.kind = PlanNodeKind::kUseDatabase;
            node.detail = "use database " + use_db->database_name;
            return common::MakeSuccess(std::move(node));
        }

        if (const auto *create_table =
                std::get_if<parser::CreateTableStatement>(&statement);
            create_table != nullptr) {
            node.kind = PlanNodeKind::kCreateTable;
            node.detail = "create table " +
                          QualifiedNameToString(create_table->table_name);
            return common::MakeSuccess(std::move(node));
        }

        if (const auto *drop_table =
                std::get_if<parser::DropTableStatement>(&statement);
            drop_table != nullptr) {
            node.kind = PlanNodeKind::kDropTable;
            node.detail = "drop table " +
                          QualifiedNameToString(drop_table->table_name);
            return common::MakeSuccess(std::move(node));
        }

        if (const auto *insert = std::get_if<parser::InsertStatement>(&statement);
            insert != nullptr) {
            node.kind = PlanNodeKind::kInsert;
            node.detail = "insert into " +
                          QualifiedNameToString(insert->table_name) + " rows=" +
                          std::to_string(insert->rows.size());
            return common::MakeSuccess(std::move(node));
        }

        if (const auto *update = std::get_if<parser::UpdateStatement>(&statement);
            update != nullptr) {
            node.kind = PlanNodeKind::kUpdate;
            node.detail = "update " + QualifiedNameToString(update->table_name) +
                          " assignments=" +
                          std::to_string(update->assignments.size());
            return common::MakeSuccess(std::move(node));
        }

        if (const auto *delete_stmt =
                std::get_if<parser::DeleteStatement>(&statement);
            delete_stmt != nullptr) {
            node.kind = PlanNodeKind::kDelete;
            node.detail =
                "delete from " + QualifiedNameToString(delete_stmt->table_name);
            return common::MakeSuccess(std::move(node));
        }

        if (const auto *select = std::get_if<parser::SelectStatement>(&statement);
            select != nullptr) {
            node.kind = PlanNodeKind::kProject;
            node.detail =
                "select " + SelectItemsSummary(select->items) + " from " +
                QualifiedNameToString(select->table_name);

            PlanNode scan;
            scan.kind = PlanNodeKind::kSeqScan;
            scan.detail = "scan " + QualifiedNameToString(select->table_name);

            if (select->where.has_value()) {
                PlanNode filter;
                filter.kind = PlanNodeKind::kFilter;
                filter.detail = "filter";
                filter.children.push_back(std::move(scan));
                node.children.push_back(std::move(filter));
            } else {
                node.children.push_back(std::move(scan));
            }

            if (!select->items.empty() && select->items[0].aggregate.has_value()) {
                PlanNode aggregate;
                aggregate.kind = PlanNodeKind::kAggregate;
                aggregate.detail = "aggregate";
                aggregate.children.push_back(std::move(node));
                return common::MakeSuccess(std::move(aggregate));
            }

            return common::MakeSuccess(std::move(node));
        }

        if (const auto *revert = std::get_if<parser::RevertStatement>(&statement);
            revert != nullptr) {
            node.kind = PlanNodeKind::kRevert;
            node.detail = "revert " + QualifiedNameToString(revert->table_name) +
                          " to " + revert->timestamp;
            return common::MakeSuccess(std::move(node));
        }

        if (std::holds_alternative<parser::UnknownStatement>(statement)) {
            return common::MakeError<PlanNode>(
                common::ErrorCode::kParseError,
                "cannot build plan for unknown statement");
        }

        return common::MakeError<PlanNode>(common::ErrorCode::kNotImplemented,
                                           "statement kind is not supported");
    }

} // namespace dbms::planner
