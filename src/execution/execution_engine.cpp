#include "execution/execution_engine.hpp"

// this file implements execution entry points for scans, writes, and
// aggregates.
#include <algorithm>
#include <iomanip>
#include <regex>
#include <sstream>
#include <unordered_map>

#include "catalog/schema.hpp"
#include "common/error.hpp"
#include "common/result.hpp"

namespace dbms::execution {

    namespace {

        std::string QualifiedNameToString(const parser::QualifiedName &name) {
            if (name.database_name.has_value()) {
                return *name.database_name + "." + name.object_name;
            }
            return name.object_name;
        }

        common::Result<std::string> ResolveDatabaseName(
            const core::SessionContext &session,
            const std::optional<std::string> &requested) {
            if (requested.has_value()) {
                return common::MakeSuccess(*requested);
            }
            if (session.current_database.empty()) {
                return common::MakeError<std::string>(
                    common::ErrorCode::kValidationError,
                    "no active database selected");
            }
            return common::MakeSuccess(session.current_database);
        }

        std::optional<std::size_t>
        FindColumnIndex(const catalog::TableSchema &schema,
                        const std::string &column_name) {
            for (std::size_t i = 0; i < schema.columns.size(); ++i) {
                if (schema.columns[i].name == column_name) {
                    return i;
                }
            }
            return std::nullopt;
        }

        common::Result<common::Value>
        EvaluateValueExpression(const parser::Expression &expr,
                                const catalog::TableSchema &schema,
                                const common::RowData &row);

        std::string EncodeIndexKey(const common::Value &value,
                                   common::ValueType type) {
            if (common::IsNull(value)) {
                return "N";
            }
            if (type == common::ValueType::kInt64) {
                const auto signed_value = std::get<std::int64_t>(value);
                const auto ordered_value =
                    static_cast<std::uint64_t>(signed_value) ^
                    (1ULL << 63ULL);
                std::ostringstream output;
                output << "I" << std::hex << std::setw(16) << std::setfill('0')
                       << ordered_value;
                return output.str();
            }
            return "S" + common::AsString(value);
        }

        common::Result<bool> ValidateRowAgainstSchema(
            common::RowData &row, const catalog::TableSchema &schema) {
            for (std::size_t column_index = 0; column_index < schema.columns.size();
                 ++column_index) {
                const auto &column_definition = schema.columns[column_index];
                if (common::IsNull(row.values[column_index]) &&
                    column_definition.default_value.has_value()) {
                    row.values[column_index] = *column_definition.default_value;
                }

                const bool allow_null =
                    column_definition.constraint !=
                        catalog::ColumnConstraint::kNotNull &&
                    column_definition.constraint !=
                        catalog::ColumnConstraint::kIndexed;
                if (!common::CanAssignToType(row.values[column_index],
                                             column_definition.type, allow_null)) {
                    return common::MakeError<bool>(
                        common::ErrorCode::kConstraintViolation,
                        "type/nullability constraint violation for column: " +
                            column_definition.name);
                }
            }
            return common::MakeSuccess(true);
        }

        common::Result<bool>
        RebuildIndexes(core::TableRuntime &table_runtime,
                       const std::vector<common::RowData> &rows) {
            std::unordered_map<std::string, std::unique_ptr<index::BStarPlusTree>>
                rebuilt_indexes;
            for (const auto &index_definition : table_runtime.schema.indexes) {
                rebuilt_indexes.emplace(
                    index_definition.column_name,
                    std::make_unique<index::BStarPlusTree>());
            }

            for (const auto &row : rows) {
                for (auto &[column_name, index_tree] : rebuilt_indexes) {
                    auto column_index =
                        FindColumnIndex(table_runtime.schema, column_name);
                    if (!column_index.has_value()) {
                        return common::MakeError<bool>(
                            common::ErrorCode::kSemanticError,
                            "indexed column not found in schema: " + column_name);
                    }
                    const auto &value = row.values[*column_index];
                    if (common::IsNull(value)) {
                        return common::MakeError<bool>(
                            common::ErrorCode::kConstraintViolation,
                            "INDEXED column cannot be NULL: " + column_name);
                    }
                    const std::string key = EncodeIndexKey(
                        value, table_runtime.schema.columns[*column_index].type);
                    if (!index_tree->Find(key).empty()) {
                        return common::MakeError<bool>(
                            common::ErrorCode::kConstraintViolation,
                            "duplicate value for INDEXED column: " + column_name);
                    }
                    index_tree->Insert(key, row.row_id);
                }
            }

            table_runtime.indexes = std::move(rebuilt_indexes);
            return common::MakeSuccess(true);
        }

        void InternStringValues(common::RowData &row,
                                storage::StringPool &string_pool) {
            for (auto &value : row.values) {
                if (!std::holds_alternative<std::string>(value)) {
                    continue;
                }
                value = string_pool.Intern(std::get<std::string>(value));
            }
        }

        void RecordTableSnapshot(versioning::VersionStore &version_store,
                                 const std::string &database_name,
                                 const core::TableRuntime &table_runtime,
                                 const std::string &operation) {
            const auto timestamp = version_store.Append(versioning::ChangeRecord{
                .database_name = database_name,
                .table_name = table_runtime.schema.table_name,
                .operation = operation,
                .timestamp = "",
                .snapshot_rows = table_runtime.heap->ScanAll(),
            });
            (void)timestamp;
        }

        common::Result<bool> EvaluatePredicate(const parser::Expression &expr,
                                               const catalog::TableSchema &schema,
                                               const common::RowData &row) {
            if (const auto *logical =
                    std::get_if<parser::LogicalExpression>(&expr.node);
                logical != nullptr) {
                auto left = EvaluatePredicate(*logical->left, schema, row);
                if (!left.ok()) {
                    return left;
                }
                auto right = EvaluatePredicate(*logical->right, schema, row);
                if (!right.ok()) {
                    return right;
                }
                if (logical->op == parser::LogicalOperator::kAnd) {
                    return common::MakeSuccess(*left.value && *right.value);
                }
                return common::MakeSuccess(*left.value || *right.value);
            }

            if (const auto *cmp =
                    std::get_if<parser::BinaryComparisonExpression>(&expr.node);
                cmp != nullptr) {
                auto left = EvaluateValueExpression(*cmp->left, schema, row);
                if (!left.ok()) {
                    return common::MakeError<bool>(left.error->code,
                                                   left.error->message);
                }
                auto right = EvaluateValueExpression(*cmp->right, schema, row);
                if (!right.ok()) {
                    return common::MakeError<bool>(right.error->code,
                                                   right.error->message);
                }

                if (common::GetValueType(*left.value) !=
                    common::GetValueType(*right.value)) {
                    return common::MakeError<bool>(
                        common::ErrorCode::kTypeMismatch,
                        "comparison type mismatch");
                }
                const auto compared =
                    common::CompareValues(*left.value, *right.value);
                switch (cmp->op) {
                    case parser::ComparisonOperator::kEqual:
                        return common::MakeSuccess(compared == 0);
                    case parser::ComparisonOperator::kNotEqual:
                        return common::MakeSuccess(compared != 0);
                    case parser::ComparisonOperator::kLess:
                        return common::MakeSuccess(compared < 0);
                    case parser::ComparisonOperator::kGreater:
                        return common::MakeSuccess(compared > 0);
                    case parser::ComparisonOperator::kLessEqual:
                        return common::MakeSuccess(compared <= 0);
                    case parser::ComparisonOperator::kGreaterEqual:
                        return common::MakeSuccess(compared >= 0);
                }
            }

            if (const auto *between =
                    std::get_if<parser::BetweenExpression>(&expr.node);
                between != nullptr) {
                auto value = EvaluateValueExpression(*between->value, schema, row);
                auto lower =
                    EvaluateValueExpression(*between->lower_bound, schema, row);
                auto upper =
                    EvaluateValueExpression(*between->upper_bound, schema, row);
                if (!value.ok() || !lower.ok() || !upper.ok()) {
                    return common::MakeError<bool>(common::ErrorCode::kTypeMismatch,
                                                   "BETWEEN evaluation failed");
                }
                if (common::GetValueType(*value.value) !=
                        common::GetValueType(*lower.value) ||
                    common::GetValueType(*value.value) !=
                        common::GetValueType(*upper.value)) {
                    return common::MakeError<bool>(
                        common::ErrorCode::kTypeMismatch,
                        "BETWEEN type mismatch");
                }
                const auto c1 = common::CompareValues(*value.value, *lower.value);
                const auto c2 = common::CompareValues(*value.value, *upper.value);
                return common::MakeSuccess(c1 >= 0 && c2 < 0);
            }

            if (const auto *like = std::get_if<parser::LikeExpression>(&expr.node);
                like != nullptr) {
                auto value = EvaluateValueExpression(*like->value, schema, row);
                auto pattern =
                    EvaluateValueExpression(*like->pattern, schema, row);
                if (!value.ok() || !pattern.ok()) {
                    return common::MakeError<bool>(common::ErrorCode::kTypeMismatch,
                                                   "LIKE evaluation failed");
                }
                if (common::GetValueType(*value.value) != common::ValueType::kString ||
                    common::GetValueType(*pattern.value) !=
                        common::ValueType::kString) {
                    return common::MakeError<bool>(
                        common::ErrorCode::kTypeMismatch,
                        "LIKE accepts only string operands");
                }
                const std::regex re(common::AsString(*pattern.value));
                return common::MakeSuccess(
                    std::regex_match(common::AsString(*value.value), re));
            }

            auto truthy = EvaluateValueExpression(expr, schema, row);
            if (!truthy.ok()) {
                return common::MakeError<bool>(truthy.error->code,
                                               truthy.error->message);
            }
            if (common::IsNull(*truthy.value)) {
                return common::MakeSuccess(false);
            }
            if (common::GetValueType(*truthy.value) == common::ValueType::kInt64) {
                return common::MakeSuccess(std::get<std::int64_t>(*truthy.value) !=
                                           0);
            }
            return common::MakeSuccess(!common::AsString(*truthy.value).empty());
        }

        common::Result<common::Value>
        EvaluateValueExpression(const parser::Expression &expr,
                                const catalog::TableSchema &schema,
                                const common::RowData &row) {
            if (const auto *literal =
                    std::get_if<parser::LiteralExpression>(&expr.node);
                literal != nullptr) {
                return common::MakeSuccess(literal->value);
            }

            if (const auto *column =
                    std::get_if<parser::ColumnReferenceExpression>(&expr.node);
                column != nullptr) {
                auto idx = FindColumnIndex(schema, column->column_name);
                if (!idx.has_value()) {
                    return common::MakeError<common::Value>(
                        common::ErrorCode::kSemanticError,
                        "unknown column: " + column->column_name);
                }
                return common::MakeSuccess(row.values[*idx]);
            }

            return common::MakeError<common::Value>(
                common::ErrorCode::kNotImplemented,
                "expression is not a scalar value expression");
        }

        common::Result<std::vector<common::RowData>>
        FilterRows(const std::vector<common::RowData> &rows,
                   const std::optional<parser::Expression> &where_clause,
                   const catalog::TableSchema &schema,
                   const core::TableRuntime *table_runtime,
                   bool *used_index_path) {
            std::vector<common::RowData> filtered;
            if (used_index_path != nullptr) {
                *used_index_path = false;
            }

            std::unordered_map<common::RowId, const common::RowData *> row_lookup;
            row_lookup.reserve(rows.size());
            for (const auto &row : rows) {
                row_lookup.emplace(row.row_id, &row);
            }

            auto append_if_passes = [&](common::RowId row_id) -> common::Result<bool> {
                const auto lookup_it = row_lookup.find(row_id);
                if (lookup_it == row_lookup.end()) {
                    return common::MakeSuccess(true);
                }
                const auto &row = *lookup_it->second;
                if (where_clause.has_value()) {
                    auto pass = EvaluatePredicate(*where_clause, schema, row);
                    if (!pass.ok()) {
                        return common::MakeError<bool>(pass.error->code,
                                                       pass.error->message);
                    }
                    if (!*pass.value) {
                        return common::MakeSuccess(true);
                    }
                }
                filtered.push_back(row);
                return common::MakeSuccess(true);
            };

            if (where_clause.has_value() && table_runtime != nullptr) {
                if (const auto *comparison =
                        std::get_if<parser::BinaryComparisonExpression>(
                            &where_clause->node);
                    comparison != nullptr) {
                    const auto *column =
                        std::get_if<parser::ColumnReferenceExpression>(
                            &comparison->left->node);
                    const auto *literal =
                        std::get_if<parser::LiteralExpression>(
                            &comparison->right->node);
                    if (column != nullptr && literal != nullptr) {
                        auto column_index =
                            FindColumnIndex(schema, column->column_name);
                        auto index_it =
                            table_runtime->indexes.find(column->column_name);
                        if (column_index.has_value() &&
                            index_it != table_runtime->indexes.end()) {
                            const auto encoded =
                                EncodeIndexKey(literal->value,
                                               schema.columns[*column_index].type);
                            std::vector<common::RowId> candidate_row_ids;
                            switch (comparison->op) {
                                case parser::ComparisonOperator::kEqual:
                                    candidate_row_ids = index_it->second->Find(encoded);
                                    break;
                                case parser::ComparisonOperator::kLess:
                                case parser::ComparisonOperator::kLessEqual: {
                                    auto entries = index_it->second->RangeScan("",
                                                                               encoded + "\xFF");
                                    for (const auto &entry : entries) {
                                        candidate_row_ids.insert(candidate_row_ids.end(),
                                                                 entry.row_ids.begin(),
                                                                 entry.row_ids.end());
                                    }
                                    break;
                                }
                                case parser::ComparisonOperator::kGreater:
                                case parser::ComparisonOperator::kGreaterEqual: {
                                    auto entries = index_it->second->RangeScan(encoded,
                                                                               "\xFF\xFF\xFF\xFF");
                                    for (const auto &entry : entries) {
                                        candidate_row_ids.insert(candidate_row_ids.end(),
                                                                 entry.row_ids.begin(),
                                                                 entry.row_ids.end());
                                    }
                                    break;
                                }
                                case parser::ComparisonOperator::kNotEqual:
                                    break;
                            }
                            for (auto row_id : candidate_row_ids) {
                                auto append_result = append_if_passes(row_id);
                                if (!append_result.ok()) {
                                    return common::MakeError<
                                        std::vector<common::RowData>>(
                                        append_result.error->code,
                                        append_result.error->message);
                                }
                            }
                            if (comparison->op !=
                                parser::ComparisonOperator::kNotEqual) {
                                if (used_index_path != nullptr) {
                                    *used_index_path = true;
                                }
                                return common::MakeSuccess(std::move(filtered));
                            }
                        }
                    }
                }

                if (const auto *between =
                        std::get_if<parser::BetweenExpression>(&where_clause->node);
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
                        auto column_index =
                            FindColumnIndex(schema, column->column_name);
                        auto index_it =
                            table_runtime->indexes.find(column->column_name);
                        if (column_index.has_value() &&
                            index_it != table_runtime->indexes.end()) {
                            auto lower_key =
                                EncodeIndexKey(lower->value,
                                               schema.columns[*column_index].type);
                            auto upper_key =
                                EncodeIndexKey(upper->value,
                                               schema.columns[*column_index].type);
                            auto entries =
                                index_it->second->RangeScan(lower_key, upper_key);
                            for (const auto &entry : entries) {
                                for (auto row_id : entry.row_ids) {
                                    auto append_result = append_if_passes(row_id);
                                    if (!append_result.ok()) {
                                        return common::MakeError<
                                            std::vector<common::RowData>>(
                                            append_result.error->code,
                                            append_result.error->message);
                                    }
                                }
                            }
                            if (used_index_path != nullptr) {
                                *used_index_path = true;
                            }
                            return common::MakeSuccess(std::move(filtered));
                        }
                    }
                }
            }

            for (const auto &row : rows) {
                if (where_clause.has_value()) {
                    auto pass = EvaluatePredicate(*where_clause, schema, row);
                    if (!pass.ok()) {
                        return common::MakeError<std::vector<common::RowData>>(
                            pass.error->code, pass.error->message);
                    }
                    if (!*pass.value) {
                        continue;
                    }
                }
                filtered.push_back(row);
            }
            return common::MakeSuccess(std::move(filtered));
        }

    } // namespace

    ExecutionEngine::ExecutionEngine(catalog::Catalog &catalog,
                                     core::RuntimeState &runtime_state,
                                     versioning::VersionStore &version_store,
                                     storage::StringPool &string_pool)
        : catalog_(catalog), runtime_state_(runtime_state),
          version_store_(version_store), string_pool_(string_pool) {}

    common::Result<QueryResult>
    ExecutionEngine::Execute(core::SessionContext &session,
                             const planner::PlanNode &plan) {
        if (!plan.statement) {
            return common::MakeError<QueryResult>(
                common::ErrorCode::kValidationError,
                "plan has no attached statement");
        }

        const auto &statement = *plan.statement;
        QueryResult result;

        if (const auto *create_db =
                std::get_if<parser::CreateDatabaseStatement>(&statement);
            create_db != nullptr) {
            if (runtime_state_.databases.contains(create_db->database_name)) {
                return common::MakeError<QueryResult>(
                    common::ErrorCode::kAlreadyExists,
                    "database already exists: " + create_db->database_name);
            }
            catalog_.CreateDatabase(create_db->database_name);
            core::DatabaseRuntime db;
            db.name = create_db->database_name;
            runtime_state_.databases.emplace(create_db->database_name,
                                             std::move(db));
            result.message = "database created";
            return common::MakeSuccess(std::move(result));
        }

        if (const auto *use_db =
                std::get_if<parser::UseDatabaseStatement>(&statement);
            use_db != nullptr) {
            if (!runtime_state_.databases.contains(use_db->database_name)) {
                return common::MakeError<QueryResult>(
                    common::ErrorCode::kNotFound,
                    "database not found: " + use_db->database_name);
            }
            session.current_database = use_db->database_name;
            result.message = "database selected";
            return common::MakeSuccess(std::move(result));
        }

        if (const auto *drop_db =
                std::get_if<parser::DropDatabaseStatement>(&statement);
            drop_db != nullptr) {
            if (!runtime_state_.databases.contains(drop_db->database_name)) {
                return common::MakeError<QueryResult>(
                    common::ErrorCode::kNotFound,
                    "database not found: " + drop_db->database_name);
            }
            runtime_state_.databases.erase(drop_db->database_name);
            catalog_.DropDatabase(drop_db->database_name);
            if (session.current_database == drop_db->database_name) {
                session.current_database.clear();
            }
            result.message = "database dropped";
            return common::MakeSuccess(std::move(result));
        }

        if (const auto *create_table =
                std::get_if<parser::CreateTableStatement>(&statement);
            create_table != nullptr) {
            auto db_name =
                ResolveDatabaseName(session, create_table->table_name.database_name);
            if (!db_name.ok()) {
                return common::MakeError<QueryResult>(db_name.error->code,
                                                      db_name.error->message);
            }
            auto db_it = runtime_state_.databases.find(*db_name.value);
            if (db_it == runtime_state_.databases.end()) {
                return common::MakeError<QueryResult>(
                    common::ErrorCode::kNotFound,
                    "database not found: " + *db_name.value);
            }
            if (db_it->second.tables.contains(create_table->table_name.object_name)) {
                return common::MakeError<QueryResult>(
                    common::ErrorCode::kAlreadyExists,
                    "table already exists: " + create_table->table_name.object_name);
            }

            catalog::TableSchema schema;
            schema.database_name = *db_name.value;
            schema.table_name = create_table->table_name.object_name;
            for (const auto &col : create_table->columns) {
                catalog::ColumnDefinition out_col;
                out_col.name = col.name;
                out_col.type = col.type;
                if (col.indexed) {
                    out_col.constraint = catalog::ColumnConstraint::kIndexed;
                } else if (col.not_null) {
                    out_col.constraint = catalog::ColumnConstraint::kNotNull;
                } else {
                    out_col.constraint = catalog::ColumnConstraint::kNone;
                }
                out_col.default_value = col.default_value;
                schema.columns.push_back(std::move(out_col));
                if (col.indexed) {
                    schema.indexes.push_back(catalog::IndexDefinition{
                        .name = "idx_" + create_table->table_name.object_name + "_" +
                                col.name,
                        .column_name = col.name,
                        .unique = true,
                        .is_primary_access_path = true,
                    });
                }
            }

            core::TableRuntime table_runtime;
            table_runtime.schema = schema;
            table_runtime.heap = std::make_unique<storage::TableHeap>(schema);
            for (const auto &index : schema.indexes) {
                table_runtime.indexes.emplace(
                    index.column_name, std::make_unique<index::BStarPlusTree>());
            }
            db_it->second.tables.emplace(schema.table_name,
                                         std::move(table_runtime));
            RecordTableSnapshot(version_store_, *db_name.value,
                                db_it->second.tables.at(schema.table_name),
                                "CREATE_TABLE");
            catalog_.RegisterTable(std::move(schema));
            result.message = "table created";
            return common::MakeSuccess(std::move(result));
        }

        if (const auto *drop_table =
                std::get_if<parser::DropTableStatement>(&statement);
            drop_table != nullptr) {
            auto db_name =
                ResolveDatabaseName(session, drop_table->table_name.database_name);
            if (!db_name.ok()) {
                return common::MakeError<QueryResult>(db_name.error->code,
                                                      db_name.error->message);
            }
            auto db_it = runtime_state_.databases.find(*db_name.value);
            if (db_it == runtime_state_.databases.end()) {
                return common::MakeError<QueryResult>(
                    common::ErrorCode::kNotFound,
                    "database not found: " + *db_name.value);
            }
            if (!db_it->second.tables.contains(drop_table->table_name.object_name)) {
                return common::MakeError<QueryResult>(
                    common::ErrorCode::kNotFound,
                    "table not found: " + drop_table->table_name.object_name);
            }
            db_it->second.tables.erase(drop_table->table_name.object_name);
            result.message = "table dropped";
            return common::MakeSuccess(std::move(result));
        }

        if (const auto *insert = std::get_if<parser::InsertStatement>(&statement);
            insert != nullptr) {
            auto db_name =
                ResolveDatabaseName(session, insert->table_name.database_name);
            if (!db_name.ok()) {
                return common::MakeError<QueryResult>(db_name.error->code,
                                                      db_name.error->message);
            }
            auto db_it = runtime_state_.databases.find(*db_name.value);
            if (db_it == runtime_state_.databases.end()) {
                return common::MakeError<QueryResult>(
                    common::ErrorCode::kNotFound,
                    "database not found: " + *db_name.value);
            }
            auto table_it = db_it->second.tables.find(insert->table_name.object_name);
            if (table_it == db_it->second.tables.end()) {
                return common::MakeError<QueryResult>(
                    common::ErrorCode::kNotFound,
                    "table not found: " + insert->table_name.object_name);
            }

            auto &table = table_it->second;
            std::vector<std::size_t> input_to_schema;
            input_to_schema.reserve(insert->column_names.size());
            for (const auto &name : insert->column_names) {
                auto idx = FindColumnIndex(table.schema, name);
                if (!idx.has_value()) {
                    return common::MakeError<QueryResult>(
                        common::ErrorCode::kSemanticError,
                        "unknown column in INSERT: " + name);
                }
                input_to_schema.push_back(*idx);
            }

            for (const auto &input_row : insert->rows) {
                common::RowData row;
                row.values.resize(table.schema.columns.size(), std::monostate{});

                for (std::size_t i = 0; i < input_row.size(); ++i) {
                    auto literal = std::get_if<parser::LiteralExpression>(
                        &input_row[i].node);
                    if (literal == nullptr) {
                        return common::MakeError<QueryResult>(
                            common::ErrorCode::kValidationError,
                            "INSERT currently accepts only literal values");
                    }
                    row.values[input_to_schema[i]] = literal->value;
                }
                InternStringValues(row, string_pool_);

                auto row_validation = ValidateRowAgainstSchema(row, table.schema);
                if (!row_validation.ok()) {
                    return common::MakeError<QueryResult>(
                        row_validation.error->code, row_validation.error->message);
                }

                for (const auto &[column, index_ptr] : table.indexes) {
                    auto idx = FindColumnIndex(table.schema, column);
                    if (!idx.has_value()) {
                        continue;
                    }
                    const auto &value = row.values[*idx];
                    if (common::IsNull(value)) {
                        return common::MakeError<QueryResult>(
                            common::ErrorCode::kConstraintViolation,
                            "INDEXED column cannot be NULL: " + column);
                    }
                    const auto encoded_key =
                        EncodeIndexKey(value, table.schema.columns[*idx].type);
                    if (!index_ptr->Find(encoded_key).empty()) {
                        return common::MakeError<QueryResult>(
                            common::ErrorCode::kConstraintViolation,
                            "duplicate value for INDEXED column: " + column);
                    }
                }

                const auto row_id = table.heap->Insert(row);
                for (const auto &[column, index_ptr] : table.indexes) {
                    auto idx = FindColumnIndex(table.schema, column);
                    if (idx.has_value()) {
                        index_ptr->Insert(
                            EncodeIndexKey(row.values[*idx],
                                           table.schema.columns[*idx].type),
                            row_id);
                    }
                }
            }
            RecordTableSnapshot(version_store_, *db_name.value, table, "INSERT");
            result.message = "rows inserted";
            return common::MakeSuccess(std::move(result));
        }

        if (const auto *update_statement =
                std::get_if<parser::UpdateStatement>(&statement);
            update_statement != nullptr) {
            auto database_name = ResolveDatabaseName(
                session, update_statement->table_name.database_name);
            if (!database_name.ok()) {
                return common::MakeError<QueryResult>(database_name.error->code,
                                                      database_name.error->message);
            }
            auto database_it = runtime_state_.databases.find(*database_name.value);
            if (database_it == runtime_state_.databases.end()) {
                return common::MakeError<QueryResult>(
                    common::ErrorCode::kNotFound,
                    "database not found: " + *database_name.value);
            }
            auto table_it = database_it->second.tables.find(
                update_statement->table_name.object_name);
            if (table_it == database_it->second.tables.end()) {
                return common::MakeError<QueryResult>(
                    common::ErrorCode::kNotFound,
                    "table not found: " +
                        update_statement->table_name.object_name);
            }

            auto &table_runtime = table_it->second;
            auto rows = table_runtime.heap->ScanAll();
            std::size_t updated_count = 0;
            for (auto &row : rows) {
                bool should_update = true;
                if (update_statement->where.has_value()) {
                    auto predicate_result = EvaluatePredicate(
                        *update_statement->where, table_runtime.schema, row);
                    if (!predicate_result.ok()) {
                        return common::MakeError<QueryResult>(
                            predicate_result.error->code,
                            predicate_result.error->message);
                    }
                    should_update = *predicate_result.value;
                }
                if (!should_update) {
                    continue;
                }

                for (const auto &assignment : update_statement->assignments) {
                    auto column_index = FindColumnIndex(table_runtime.schema,
                                                        assignment.column_name);
                    if (!column_index.has_value()) {
                        return common::MakeError<QueryResult>(
                            common::ErrorCode::kSemanticError,
                            "unknown column in UPDATE: " +
                                assignment.column_name);
                    }
                    auto value_result = EvaluateValueExpression(
                        assignment.value, table_runtime.schema, row);
                    if (!value_result.ok()) {
                        return common::MakeError<QueryResult>(
                            value_result.error->code,
                            value_result.error->message);
                    }
                    row.values[*column_index] = *value_result.value;
                }
                InternStringValues(row, string_pool_);

                auto row_validation =
                    ValidateRowAgainstSchema(row, table_runtime.schema);
                if (!row_validation.ok()) {
                    return common::MakeError<QueryResult>(
                        row_validation.error->code,
                        row_validation.error->message);
                }
                ++updated_count;
            }

            auto index_rebuild = RebuildIndexes(table_runtime, rows);
            if (!index_rebuild.ok()) {
                return common::MakeError<QueryResult>(
                    index_rebuild.error->code, index_rebuild.error->message);
            }
            table_runtime.heap->ReplaceAll(std::move(rows));
            RecordTableSnapshot(version_store_, *database_name.value,
                                table_runtime, "UPDATE");
            result.message =
                "updated " + std::to_string(updated_count) + " row(s)";
            return common::MakeSuccess(std::move(result));
        }

        if (const auto *delete_statement =
                std::get_if<parser::DeleteStatement>(&statement);
            delete_statement != nullptr) {
            auto database_name =
                ResolveDatabaseName(session, delete_statement->table_name.database_name);
            if (!database_name.ok()) {
                return common::MakeError<QueryResult>(database_name.error->code,
                                                      database_name.error->message);
            }
            auto database_it = runtime_state_.databases.find(*database_name.value);
            if (database_it == runtime_state_.databases.end()) {
                return common::MakeError<QueryResult>(
                    common::ErrorCode::kNotFound,
                    "database not found: " + *database_name.value);
            }
            auto table_it = database_it->second.tables.find(
                delete_statement->table_name.object_name);
            if (table_it == database_it->second.tables.end()) {
                return common::MakeError<QueryResult>(
                    common::ErrorCode::kNotFound,
                    "table not found: " +
                        delete_statement->table_name.object_name);
            }

            auto &table_runtime = table_it->second;
            auto rows = table_runtime.heap->ScanAll();
            std::vector<common::RowData> kept_rows;
            kept_rows.reserve(rows.size());
            std::size_t deleted_count = 0;
            for (const auto &row : rows) {
                bool should_delete = true;
                if (delete_statement->where.has_value()) {
                    auto predicate_result = EvaluatePredicate(
                        *delete_statement->where, table_runtime.schema, row);
                    if (!predicate_result.ok()) {
                        return common::MakeError<QueryResult>(
                            predicate_result.error->code,
                            predicate_result.error->message);
                    }
                    should_delete = *predicate_result.value;
                }
                if (should_delete) {
                    ++deleted_count;
                    continue;
                }
                kept_rows.push_back(row);
            }

            auto index_rebuild = RebuildIndexes(table_runtime, kept_rows);
            if (!index_rebuild.ok()) {
                return common::MakeError<QueryResult>(
                    index_rebuild.error->code, index_rebuild.error->message);
            }
            table_runtime.heap->ReplaceAll(std::move(kept_rows));
            RecordTableSnapshot(version_store_, *database_name.value,
                                table_runtime, "DELETE");
            result.message =
                "deleted " + std::to_string(deleted_count) + " row(s)";
            return common::MakeSuccess(std::move(result));
        }

        if (const auto *select = std::get_if<parser::SelectStatement>(&statement);
            select != nullptr) {
            auto db_name =
                ResolveDatabaseName(session, select->table_name.database_name);
            if (!db_name.ok()) {
                return common::MakeError<QueryResult>(db_name.error->code,
                                                      db_name.error->message);
            }
            auto db_it = runtime_state_.databases.find(*db_name.value);
            if (db_it == runtime_state_.databases.end()) {
                return common::MakeError<QueryResult>(
                    common::ErrorCode::kNotFound,
                    "database not found: " + *db_name.value);
            }
            auto table_it = db_it->second.tables.find(select->table_name.object_name);
            if (table_it == db_it->second.tables.end()) {
                return common::MakeError<QueryResult>(
                    common::ErrorCode::kNotFound,
                    "table not found: " + select->table_name.object_name);
            }

            const auto &table = table_it->second;
            const auto rows = table.heap->ScanAll();
            auto filtered_rows =
                FilterRows(rows, select->where, table.schema, &table,
                           /*used_index_path=*/nullptr);
            if (!filtered_rows.ok()) {
                return common::MakeError<QueryResult>(
                    filtered_rows.error->code, filtered_rows.error->message);
            }
            const bool is_aggregate_query =
                !select->items.empty() && select->items[0].aggregate.has_value();
            if (is_aggregate_query) {
                common::RowData aggregate_row;
                for (const auto &item : select->items) {
                    if (!item.aggregate.has_value()) {
                        return common::MakeError<QueryResult>(
                            common::ErrorCode::kSemanticError,
                            "aggregate SELECT cannot contain non-aggregate items");
                    }

                    std::optional<std::size_t> column_index = std::nullopt;
                    if (item.column_name != "*") {
                        column_index =
                            FindColumnIndex(table.schema, item.column_name);
                        if (!column_index.has_value()) {
                            return common::MakeError<QueryResult>(
                                common::ErrorCode::kSemanticError,
                                "unknown column in SELECT: " + item.column_name);
                        }
                    }

                    if (item.alias.has_value()) {
                        result.column_names.push_back(*item.alias);
                    } else if (item.column_name == "*") {
                        result.column_names.push_back("count");
                    } else {
                        result.column_names.push_back(item.column_name);
                    }

                    switch (*item.aggregate) {
                        case parser::AggregateKind::kCount: {
                            std::int64_t count = 0;
                            for (const auto &row : *filtered_rows.value) {
                                if (item.column_name == "*") {
                                    ++count;
                                } else if (!common::IsNull(
                                               row.values[*column_index])) {
                                    ++count;
                                }
                            }
                            aggregate_row.values.push_back(count);
                            break;
                        }
                        case parser::AggregateKind::kSum:
                        case parser::AggregateKind::kAvg: {
                            if (item.column_name == "*") {
                                return common::MakeError<QueryResult>(
                                    common::ErrorCode::kSemanticError,
                                    "SUM/AVG do not support wildcard '*'");
                            }
                            std::int64_t sum = 0;
                            std::int64_t count = 0;
                            for (const auto &row : *filtered_rows.value) {
                                const auto &value = row.values[*column_index];
                                if (common::IsNull(value)) {
                                    continue;
                                }
                                if (common::GetValueType(value) !=
                                    common::ValueType::kInt64) {
                                    return common::MakeError<QueryResult>(
                                        common::ErrorCode::kTypeMismatch,
                                        "SUM/AVG require INT column values");
                                }
                                sum += std::get<std::int64_t>(value);
                                ++count;
                            }

                            if (*item.aggregate ==
                                parser::AggregateKind::kSum) {
                                aggregate_row.values.push_back(sum);
                            } else {
                                if (count == 0) {
                                    aggregate_row.values.push_back(
                                        std::monostate{});
                                } else {
                                    aggregate_row.values.push_back(sum / count);
                                }
                            }
                            break;
                        }
                    }
                }
                result.rows.push_back(std::move(aggregate_row));
                result.message = "selected 1 row(s)";
                return common::MakeSuccess(std::move(result));
            }

            for (const auto &row : *filtered_rows.value) {

                common::RowData out;
                if (select->items.size() == 1 && select->items[0].is_wildcard) {
                    out = row;
                    if (result.column_names.empty()) {
                        for (const auto &column : table.schema.columns) {
                            result.column_names.push_back(column.name);
                        }
                    }
                } else {
                    if (result.column_names.empty()) {
                        for (const auto &item : select->items) {
                            if (item.alias.has_value()) {
                                result.column_names.push_back(*item.alias);
                            } else {
                                result.column_names.push_back(item.column_name);
                            }
                        }
                    }
                    for (const auto &item : select->items) {
                        auto idx = FindColumnIndex(table.schema, item.column_name);
                        if (!idx.has_value()) {
                            return common::MakeError<QueryResult>(
                                common::ErrorCode::kSemanticError,
                                "unknown column in SELECT: " + item.column_name);
                        }
                        out.values.push_back(row.values[*idx]);
                    }
                }
                result.rows.push_back(std::move(out));
            }
            result.message = "selected " + std::to_string(result.rows.size()) +
                             " row(s)";
            return common::MakeSuccess(std::move(result));
        }

        if (const auto *revert = std::get_if<parser::RevertStatement>(&statement);
            revert != nullptr) {
            auto database_name =
                ResolveDatabaseName(session, revert->table_name.database_name);
            if (!database_name.ok()) {
                return common::MakeError<QueryResult>(database_name.error->code,
                                                      database_name.error->message);
            }
            auto database_it = runtime_state_.databases.find(*database_name.value);
            if (database_it == runtime_state_.databases.end()) {
                return common::MakeError<QueryResult>(
                    common::ErrorCode::kNotFound,
                    "database not found: " + *database_name.value);
            }
            auto table_it =
                database_it->second.tables.find(revert->table_name.object_name);
            if (table_it == database_it->second.tables.end()) {
                return common::MakeError<QueryResult>(
                    common::ErrorCode::kNotFound,
                    "table not found: " + revert->table_name.object_name);
            }

            std::optional<versioning::ChangeRecord> snapshot;
            if (revert->mode == parser::RevertStatement::RevertMode::kLatest) {
                snapshot = version_store_.LatestSnapshot(
                    *database_name.value, revert->table_name.object_name);
            } else {
                static const std::regex timestamp_pattern(
                    R"(^\d{4}\.\d{2}\.\d{2}-\d{2}:\d{2}:\d{2}\.\d{6}$)");
                if (!std::regex_match(revert->timestamp, timestamp_pattern)) {
                    return common::MakeError<QueryResult>(
                        common::ErrorCode::kValidationError,
                        "invalid REVERT timestamp format, expected YYYY.MM.DD-HH:MM:SS.ffffff");
                }

                if (revert->mode ==
                    parser::RevertStatement::RevertMode::kExact) {
                    snapshot = version_store_.SnapshotExact(
                        *database_name.value, revert->table_name.object_name,
                        revert->timestamp);
                } else {
                    snapshot = version_store_.SnapshotAtOrBefore(
                        *database_name.value, revert->table_name.object_name,
                        revert->timestamp);
                }
            }

            if (!snapshot.has_value()) {
                return common::MakeError<QueryResult>(
                    common::ErrorCode::kNotFound,
                    "no snapshot available for REVERT");
            }

            auto &table_runtime = table_it->second;
            for (const auto &row : snapshot->snapshot_rows) {
                if (row.values.size() != table_runtime.schema.columns.size()) {
                    return common::MakeError<QueryResult>(
                        common::ErrorCode::kValidationError,
                        "snapshot row shape mismatch for table schema");
                }
            }

            auto index_rebuild =
                RebuildIndexes(table_runtime, snapshot->snapshot_rows);
            if (!index_rebuild.ok()) {
                return common::MakeError<QueryResult>(
                    index_rebuild.error->code, index_rebuild.error->message);
            }
            table_runtime.heap->ReplaceAll(snapshot->snapshot_rows);
            RecordTableSnapshot(version_store_, *database_name.value,
                                table_runtime, "REVERT");
            result.message = "reverted to " + snapshot->timestamp;
            return common::MakeSuccess(std::move(result));
        }

        if (std::holds_alternative<parser::CheckIndexStatement>(statement)) {
            auto check = core::ValidateRuntimeIndexConsistency(runtime_state_);
            if (!check.ok()) {
                return common::MakeError<QueryResult>(check.error->code,
                                                      check.error->message);
            }
            result.message = "index check passed";
            return common::MakeSuccess(std::move(result));
        }

        if (std::holds_alternative<parser::RebuildIndexStatement>(statement)) {
            auto rebuilt = core::RebuildRuntimeIndexes(runtime_state_);
            if (!rebuilt.ok()) {
                return common::MakeError<QueryResult>(rebuilt.error->code,
                                                      rebuilt.error->message);
            }
            result.message = "index rebuild completed";
            return common::MakeSuccess(std::move(result));
        }

        return common::MakeError<QueryResult>(
            common::ErrorCode::kNotImplemented,
            "plan kind is not implemented: " + plan.detail);
    }

} // namespace dbms::execution
