#include "core/runtime_persistence.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "catalog/schema.hpp"
#include "common/types.hpp"

namespace dbms::core {

    namespace {

        std::string Escape(std::string value) {
            std::string escaped;
            escaped.reserve(value.size());
            for (char ch : value) {
                if (ch == '\\' || ch == '\t' || ch == '\n') {
                    escaped.push_back('\\');
                }
                escaped.push_back(ch);
            }
            return escaped;
        }

        std::string Unescape(const std::string &value) {
            std::string unescaped;
            unescaped.reserve(value.size());
            bool escaped = false;
            for (char ch : value) {
                if (escaped) {
                    unescaped.push_back(ch);
                    escaped = false;
                } else if (ch == '\\') {
                    escaped = true;
                } else {
                    unescaped.push_back(ch);
                }
            }
            return unescaped;
        }

        std::vector<std::string> SplitByTab(const std::string &line) {
            std::vector<std::string> parts;
            std::string current;
            bool escaped = false;
            for (char ch : line) {
                if (escaped) {
                    current.push_back(ch);
                    escaped = false;
                    continue;
                }
                if (ch == '\\') {
                    escaped = true;
                    current.push_back(ch);
                    continue;
                }
                if (ch == '\t') {
                    parts.push_back(current);
                    current.clear();
                    continue;
                }
                current.push_back(ch);
            }
            parts.push_back(current);
            return parts;
        }

        std::string EncodeValue(const common::Value &value) {
            if (std::holds_alternative<std::monostate>(value)) {
                return "N";
            }
            if (std::holds_alternative<std::int64_t>(value)) {
                return "I:" + std::to_string(std::get<std::int64_t>(value));
            }
            return "S:" + Escape(std::get<std::string>(value));
        }

        common::Value DecodeValue(const std::string &encoded) {
            if (encoded == "N") {
                return std::monostate{};
            }
            if (encoded.rfind("I:", 0) == 0) {
                return static_cast<std::int64_t>(std::stoll(encoded.substr(2)));
            }
            if (encoded.rfind("S:", 0) == 0) {
                return Unescape(encoded.substr(2));
            }
            return std::monostate{};
        }

    } // namespace

    RuntimePersistence::RuntimePersistence(std::string root_path)
        : root_path_(std::move(root_path)) {}

    std::string RuntimePersistence::StateFilePath() const {
        return root_path_ + "/runtime_state.tsv";
    }

    bool RuntimePersistence::Save(const RuntimeState &runtime_state) const {
        std::filesystem::create_directories(root_path_);
        std::ofstream output(StateFilePath(), std::ios::trunc);
        if (!output.is_open()) {
            return false;
        }

        for (const auto &[database_name, database_runtime] :
             runtime_state.databases) {
            output << "DB\t" << Escape(database_name) << "\n";
            for (const auto &[table_name, table_runtime] : database_runtime.tables) {
                output << "TABLE\t" << Escape(database_name) << "\t"
                       << Escape(table_name) << "\n";
                for (const auto &column : table_runtime.schema.columns) {
                    output << "COLUMN\t" << Escape(database_name) << "\t"
                           << Escape(table_name) << "\t" << Escape(column.name)
                           << "\t" << static_cast<int>(column.type) << "\t"
                           << static_cast<int>(column.constraint) << "\t";
                    if (column.default_value.has_value()) {
                        output << "1\t" << EncodeValue(*column.default_value);
                    } else {
                        output << "0\tN";
                    }
                    output << "\n";
                }

                const auto rows = table_runtime.heap->ScanAll();
                for (const auto &row : rows) {
                    output << "ROW\t" << Escape(database_name) << "\t"
                           << Escape(table_name);
                    for (const auto &value : row.values) {
                        output << "\t" << EncodeValue(value);
                    }
                    output << "\n";
                }
            }
        }
        return true;
    }

    bool RuntimePersistence::Load(RuntimeState &runtime_state) const {
        std::ifstream input(StateFilePath());
        if (!input.is_open()) {
            return true;
        }

        runtime_state.databases.clear();
        std::string line;
        while (std::getline(input, line)) {
            if (line.empty()) {
                continue;
            }
            const auto parts = SplitByTab(line);
            if (parts.empty()) {
                continue;
            }

            if (parts[0] == "DB" && parts.size() >= 2) {
                const std::string database_name = Unescape(parts[1]);
                runtime_state.databases.emplace(
                    database_name,
                    DatabaseRuntime{.name = database_name, .tables = {}});
                continue;
            }

            if (parts[0] == "TABLE" && parts.size() >= 3) {
                const std::string database_name = Unescape(parts[1]);
                const std::string table_name = Unescape(parts[2]);
                auto database_it = runtime_state.databases.find(database_name);
                if (database_it == runtime_state.databases.end()) {
                    runtime_state.databases.emplace(
                        database_name,
                        DatabaseRuntime{.name = database_name, .tables = {}});
                    database_it = runtime_state.databases.find(database_name);
                }

                core::TableRuntime table_runtime;
                table_runtime.schema.database_name = database_name;
                table_runtime.schema.table_name = table_name;
                table_runtime.heap = std::make_unique<storage::TableHeap>(
                    table_runtime.schema);
                database_it->second.tables.emplace(table_name,
                                                   std::move(table_runtime));
                continue;
            }

            if (parts[0] == "COLUMN" && parts.size() >= 8) {
                const std::string database_name = Unescape(parts[1]);
                const std::string table_name = Unescape(parts[2]);
                const std::string column_name = Unescape(parts[3]);
                auto database_it = runtime_state.databases.find(database_name);
                if (database_it == runtime_state.databases.end()) {
                    continue;
                }
                auto table_it = database_it->second.tables.find(table_name);
                if (table_it == database_it->second.tables.end()) {
                    continue;
                }

                catalog::ColumnDefinition column;
                column.name = column_name;
                column.type = static_cast<common::ValueType>(std::stoi(parts[4]));
                column.constraint =
                    static_cast<catalog::ColumnConstraint>(std::stoi(parts[5]));
                if (parts[6] == "1") {
                    column.default_value = DecodeValue(parts[7]);
                }
                table_it->second.schema.columns.push_back(column);
                if (column.constraint == catalog::ColumnConstraint::kIndexed) {
                    table_it->second.schema.indexes.push_back(
                        catalog::IndexDefinition{
                            .name = "idx_" + table_name + "_" + column_name,
                            .column_name = column_name,
                            .unique = true,
                            .is_primary_access_path = true,
                        });
                    table_it->second.indexes.emplace(
                        column_name, std::make_unique<index::BStarPlusTree>());
                }
                table_it->second.heap =
                    std::make_unique<storage::TableHeap>(table_it->second.schema);
                continue;
            }

            if (parts[0] == "ROW" && parts.size() >= 3) {
                const std::string database_name = Unescape(parts[1]);
                const std::string table_name = Unescape(parts[2]);
                auto database_it = runtime_state.databases.find(database_name);
                if (database_it == runtime_state.databases.end()) {
                    continue;
                }
                auto table_it = database_it->second.tables.find(table_name);
                if (table_it == database_it->second.tables.end()) {
                    continue;
                }

                common::RowData row;
                for (std::size_t index = 3; index < parts.size(); ++index) {
                    row.values.push_back(DecodeValue(parts[index]));
                }
                const auto row_id = table_it->second.heap->Insert(row);
                for (const auto &[column_name, index_tree] :
                     table_it->second.indexes) {
                    for (std::size_t column_index = 0;
                         column_index < table_it->second.schema.columns.size();
                         ++column_index) {
                        if (table_it->second.schema.columns[column_index].name ==
                            column_name) {
                            index_tree->Insert(
                                common::ValueToString(
                                    row.values[column_index]),
                                row_id);
                            break;
                        }
                    }
                }
            }
        }
        return true;
    }

} // namespace dbms::core
