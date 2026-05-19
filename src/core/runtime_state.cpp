#include "core/runtime_state.hpp"

#include <iomanip>
#include <sstream>
#include <unordered_set>

#include "common/error.hpp"

namespace dbms::core {

    namespace {

        std::optional<std::size_t>
        FindColumnIndex(const catalog::TableSchema &schema,
                        const std::string &column_name) {
            for (std::size_t index = 0; index < schema.columns.size(); ++index) {
                if (schema.columns[index].name == column_name) {
                    return index;
                }
            }
            return std::nullopt;
        }

        std::string EncodeIndexKey(const common::Value &value,
                                   common::ValueType type) {
            if (std::holds_alternative<std::monostate>(value)) {
                return "N";
            }
            if (type == common::ValueType::kInt64) {
                const auto signed_value = std::get<std::int64_t>(value);
                const auto ordered_value =
                    static_cast<std::uint64_t>(signed_value) ^ (1ULL << 63ULL);
                std::ostringstream output;
                output << "I" << std::hex << std::setw(16) << std::setfill('0')
                       << ordered_value;
                return output.str();
            }
            return "S" + std::get<std::string>(value);
        }

    } // namespace

    TableRuntime CloneTableRuntime(const TableRuntime &source) {
        TableRuntime cloned;
        cloned.schema = source.schema;
        cloned.heap = std::make_unique<storage::TableHeap>(cloned.schema);
        const auto rows = source.heap->ScanAll();
        for (const auto &row : rows) {
            const auto row_id = cloned.heap->Insert(row);
            (void)row_id;
        }
        for (const auto &index_definition : cloned.schema.indexes) {
            cloned.indexes.emplace(index_definition.column_name,
                                   std::make_unique<index::BStarPlusTree>());
        }
        for (std::size_t row_index = 0; row_index < rows.size(); ++row_index) {
            const auto &row = rows[row_index];
            for (const auto &[column_name, index_tree] : cloned.indexes) {
                std::size_t column_index = 0;
                bool found = false;
                for (; column_index < cloned.schema.columns.size(); ++column_index) {
                    if (cloned.schema.columns[column_index].name == column_name) {
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    continue;
                }
                const auto &value = row.values[column_index];
                if (std::holds_alternative<std::monostate>(value)) {
                    index_tree->Insert("N", static_cast<common::RowId>(row_index));
                } else if (cloned.schema.columns[column_index].type ==
                           common::ValueType::kInt64) {
                    const auto signed_value = std::get<std::int64_t>(value);
                    const auto ordered_value =
                        static_cast<std::uint64_t>(signed_value) ^ (1ULL << 63ULL);
                    std::ostringstream output;
                    output << "I" << std::hex << std::setw(16)
                           << std::setfill('0') << ordered_value;
                    index_tree->Insert(output.str(),
                                       static_cast<common::RowId>(row_index));
                } else {
                    index_tree->Insert("S" + std::get<std::string>(value),
                                       static_cast<common::RowId>(row_index));
                }
            }
        }
        return cloned;
    }

    RuntimeState CloneRuntimeState(const RuntimeState &source) {
        RuntimeState cloned_state;
        for (const auto &[database_name, database_runtime] : source.databases) {
            DatabaseRuntime cloned_database;
            cloned_database.name = database_runtime.name;
            for (const auto &[table_name, table_runtime] : database_runtime.tables) {
                cloned_database.tables.emplace(table_name,
                                              CloneTableRuntime(table_runtime));
            }
            cloned_state.databases.emplace(database_name,
                                           std::move(cloned_database));
        }
        return cloned_state;
    }

    common::Result<bool>
    ValidateRuntimeIndexConsistency(const RuntimeState &runtime_state) {
        for (const auto &[database_name, database_runtime] : runtime_state.databases) {
            for (const auto &[table_name, table_runtime] : database_runtime.tables) {
                const auto rows = table_runtime.heap->ScanAll();
                for (const auto &[indexed_column_name, index_tree] :
                     table_runtime.indexes) {
                    if (!index_tree->ValidateCanonicalInvariants()) {
                        return common::MakeError<bool>(
                            common::ErrorCode::kStorageError,
                            "index invariants broken for " + database_name + "." +
                                table_name + "." + indexed_column_name);
                    }
                    const auto column_index =
                        FindColumnIndex(table_runtime.schema, indexed_column_name);
                    if (!column_index.has_value()) {
                        return common::MakeError<bool>(
                            common::ErrorCode::kStorageError,
                            "indexed column missing in schema: " + database_name +
                                "." + table_name + "." + indexed_column_name);
                    }

                    std::unordered_set<std::string> seen_keys;
                    for (std::size_t row_index = 0; row_index < rows.size();
                         ++row_index) {
                        const auto &value = rows[row_index].values[*column_index];
                        const auto encoded =
                            EncodeIndexKey(value,
                                           table_runtime.schema.columns[*column_index]
                                               .type);
                        if (!seen_keys.insert(encoded).second) {
                            return common::MakeError<bool>(
                                common::ErrorCode::kStorageError,
                                "duplicate indexed key in heap for " +
                                    database_name + "." + table_name + "." +
                                    indexed_column_name);
                        }
                        const auto found = index_tree->Find(encoded);
                        const auto row_id = static_cast<common::RowId>(row_index);
                        if (found.size() != 1 || found[0] != row_id) {
                            return common::MakeError<bool>(
                                common::ErrorCode::kStorageError,
                                "index lookup mismatch for " + database_name + "." +
                                    table_name + "." + indexed_column_name);
                        }
                    }
                }
            }
        }
        return common::MakeSuccess(true);
    }

} // namespace dbms::core
