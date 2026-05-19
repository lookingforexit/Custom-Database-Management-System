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

        std::optional<std::string>
        ExtractIndexedCandidateColumn(const parser::Expression &expression) {
            if (const auto *comparison =
                    std::get_if<parser::BinaryComparisonExpression>(&expression.node);
                comparison != nullptr) {
                const bool indexable_operator =
                    comparison->op == parser::ComparisonOperator::kEqual ||
                    comparison->op == parser::ComparisonOperator::kLess ||
                    comparison->op == parser::ComparisonOperator::kGreater ||
                    comparison->op == parser::ComparisonOperator::kLessEqual ||
                    comparison->op == parser::ComparisonOperator::kGreaterEqual;
                if (!indexable_operator) {
                    return std::nullopt;
                }
                const auto *column =
                    std::get_if<parser::ColumnReferenceExpression>(
                        &comparison->left->node);
                const auto *literal =
                    std::get_if<parser::LiteralExpression>(
                        &comparison->right->node);
                if (column != nullptr && literal != nullptr) {
                    return column->column_name;
                }
                return std::nullopt;
            }

            if (const auto *between =
                    std::get_if<parser::BetweenExpression>(&expression.node);
                between != nullptr) {
                const auto *column =
                    std::get_if<parser::ColumnReferenceExpression>(
                        &between->value->node);
                const auto *lower =
                    std::get_if<parser::LiteralExpression>(
                        &between->lower_bound->node);
                const auto *upper =
                    std::get_if<parser::LiteralExpression>(
                        &between->upper_bound->node);
                if (column != nullptr && lower != nullptr && upper != nullptr) {
                    return column->column_name;
                }
                return std::nullopt;
            }

            return std::nullopt;
        }

        bool TableHasIndexOnColumn(const core::RuntimeState *runtime_state,
                                   const parser::QualifiedName &table_name,
                                   const std::string &column_name) {
            if (runtime_state == nullptr) {
                return false;
            }
            if (!table_name.database_name.has_value()) {
                return false;
            }
            auto database_it =
                runtime_state->databases.find(*table_name.database_name);
            if (database_it == runtime_state->databases.end()) {
                return false;
            }
            auto table_it =
                database_it->second.tables.find(table_name.object_name);
            if (table_it == database_it->second.tables.end()) {
                return false;
            }
            return table_it->second.indexes.contains(column_name);
        }

    } // namespace

    Planner::Planner(core::RuntimeState *runtime_state)
        : runtime_state_(runtime_state) {}

    void Planner::set_runtime_state(core::RuntimeState *runtime_state) {
        runtime_state_ = runtime_state;
    }

    common::Result<PlanNode>
    Planner::BuildPlan(parser::Statement statement) const {
        PlanNode node;
        auto statement_ptr =
            std::make_shared<parser::Statement>(std::move(statement));
        const auto &statement_ref = *statement_ptr;
        node.statement = statement_ptr;

        if (const auto *create_db =
                std::get_if<parser::CreateDatabaseStatement>(&statement_ref);
            create_db != nullptr) {
            node.kind = PlanNodeKind::kCreateDatabase;
            node.detail = "create database " + create_db->database_name;
            return common::MakeSuccess(std::move(node));
        }

        if (const auto *drop_db =
                std::get_if<parser::DropDatabaseStatement>(&statement_ref);
            drop_db != nullptr) {
            node.kind = PlanNodeKind::kDropDatabase;
            node.detail = "drop database " + drop_db->database_name;
            return common::MakeSuccess(std::move(node));
        }

        if (const auto *use_db =
                std::get_if<parser::UseDatabaseStatement>(&statement_ref);
            use_db != nullptr) {
            node.kind = PlanNodeKind::kUseDatabase;
            node.detail = "use database " + use_db->database_name;
            return common::MakeSuccess(std::move(node));
        }

        if (const auto *create_table =
                std::get_if<parser::CreateTableStatement>(&statement_ref);
            create_table != nullptr) {
            node.kind = PlanNodeKind::kCreateTable;
            node.detail = "create table " +
                          QualifiedNameToString(create_table->table_name);
            return common::MakeSuccess(std::move(node));
        }

        if (const auto *drop_table =
                std::get_if<parser::DropTableStatement>(&statement_ref);
            drop_table != nullptr) {
            node.kind = PlanNodeKind::kDropTable;
            node.detail = "drop table " +
                          QualifiedNameToString(drop_table->table_name);
            return common::MakeSuccess(std::move(node));
        }

        if (const auto *insert = std::get_if<parser::InsertStatement>(&statement_ref);
            insert != nullptr) {
            node.kind = PlanNodeKind::kInsert;
            node.detail = "insert into " +
                          QualifiedNameToString(insert->table_name) + " rows=" +
                          std::to_string(insert->rows.size());
            return common::MakeSuccess(std::move(node));
        }

        if (const auto *update = std::get_if<parser::UpdateStatement>(&statement_ref);
            update != nullptr) {
            node.kind = PlanNodeKind::kUpdate;
            node.detail = "update " + QualifiedNameToString(update->table_name) +
                          " assignments=" +
                          std::to_string(update->assignments.size());
            return common::MakeSuccess(std::move(node));
        }

        if (const auto *delete_statement =
                std::get_if<parser::DeleteStatement>(&statement_ref);
            delete_statement != nullptr) {
            node.kind = PlanNodeKind::kDelete;
            node.detail =
                "delete from " + QualifiedNameToString(delete_statement->table_name);
            return common::MakeSuccess(std::move(node));
        }

        if (const auto *select = std::get_if<parser::SelectStatement>(&statement_ref);
            select != nullptr) {
            node.kind = PlanNodeKind::kProject;
            node.detail =
                "select " + SelectItemsSummary(select->items) + " from " +
                QualifiedNameToString(select->table_name);

            PlanNode scan;
            scan.kind = PlanNodeKind::kSeqScan;
            scan.detail = "scan " + QualifiedNameToString(select->table_name);
            scan.statement = statement_ptr;

            if (select->where.has_value()) {
                if (auto index_column =
                        ExtractIndexedCandidateColumn(*select->where);
                    index_column.has_value() &&
                    TableHasIndexOnColumn(runtime_state_, select->table_name,
                                          *index_column)) {
                    scan.kind = PlanNodeKind::kIndexScan;
                    scan.detail = "index scan " +
                                  QualifiedNameToString(select->table_name) +
                                  " on " + *index_column;
                }
                PlanNode filter;
                filter.kind = PlanNodeKind::kFilter;
                filter.detail = "filter";
                filter.statement = statement_ptr;
                filter.children.push_back(std::move(scan));
                node.children.push_back(std::move(filter));
            } else {
                node.children.push_back(std::move(scan));
            }

            if (!select->items.empty() && select->items[0].aggregate.has_value()) {
                PlanNode aggregate;
                aggregate.kind = PlanNodeKind::kAggregate;
                aggregate.detail = "aggregate";
                aggregate.statement = statement_ptr;
                aggregate.children.push_back(std::move(node));
                return common::MakeSuccess(std::move(aggregate));
            }

            return common::MakeSuccess(std::move(node));
        }

        if (const auto *revert = std::get_if<parser::RevertStatement>(&statement_ref);
            revert != nullptr) {
            node.kind = PlanNodeKind::kRevert;
            node.detail = "revert " + QualifiedNameToString(revert->table_name) +
                          " to " + revert->timestamp;
            return common::MakeSuccess(std::move(node));
        }

        if (std::holds_alternative<parser::UnknownStatement>(statement_ref)) {
            return common::MakeError<PlanNode>(
                common::ErrorCode::kParseError,
                "cannot build plan for unknown statement");
        }

        return common::MakeError<PlanNode>(common::ErrorCode::kNotImplemented,
                                           "statement kind is not supported");
    }

} // namespace dbms::planner
