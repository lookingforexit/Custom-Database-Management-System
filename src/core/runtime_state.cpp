#include "core/runtime_state.hpp"

#include <iomanip>
#include <sstream>

namespace dbms::core {

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

} // namespace dbms::core
