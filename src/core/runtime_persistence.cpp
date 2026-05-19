#include "core/runtime_persistence.hpp"

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "catalog/schema.hpp"
#include "common/types.hpp"

namespace dbms::core {

    namespace {

        constexpr std::string_view kFormatTag = "FORMAT";
        constexpr std::string_view kFormatVersion = "1";

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

        bool ParseInteger(const std::string &raw, int &value) {
            try {
                std::size_t consumed = 0;
                const int parsed = std::stoi(raw, &consumed);
                if (consumed != raw.size()) {
                    return false;
                }
                value = parsed;
                return true;
            } catch (...) {
                return false;
            }
        }

        std::optional<common::Value> TryDecodeValue(const std::string &encoded) {
            if (encoded == "N") {
                return std::monostate{};
            }
            if (encoded.rfind("I:", 0) == 0) {
                try {
                    std::size_t consumed = 0;
                    const auto parsed = std::stoll(encoded.substr(2), &consumed);
                    if (consumed != encoded.size() - 2) {
                        return std::nullopt;
                    }
                    return static_cast<std::int64_t>(parsed);
                } catch (...) {
                    return std::nullopt;
                }
            }
            if (encoded.rfind("S:", 0) == 0) {
                return Unescape(encoded.substr(2));
            }
            return std::nullopt;
        }

        std::string EncodeIndexKeyForType(const common::Value &value,
                                          common::ValueType type) {
            if (common::IsNull(value)) {
                return "N";
            }
            if (type == common::ValueType::kInt64) {
                const auto signed_value = std::get<std::int64_t>(value);
                const auto ordered_value =
                    static_cast<std::uint64_t>(signed_value) ^
                    (1ULL << 63ULL);
                std::ostringstream stream;
                stream << "I" << std::hex << std::setw(16) << std::setfill('0')
                       << ordered_value;
                return stream.str();
            }
            return "S" + std::get<std::string>(value);
        }

    } // namespace

    RuntimePersistence::RuntimePersistence(std::string root_path)
        : root_path_(std::move(root_path)) {}

    std::string RuntimePersistence::StateFilePath() const {
        return root_path_ + "/runtime_state.tsv";
    }

    bool RuntimePersistence::Save(const RuntimeState &runtime_state) const {
        std::filesystem::create_directories(root_path_);
        const auto final_path = StateFilePath();
        const auto temp_path = final_path + ".tmp";
        std::ofstream output(temp_path, std::ios::trunc);
        if (!output.is_open()) {
            return false;
        }

        output << kFormatTag << "\t" << kFormatVersion << "\n";
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
        output.flush();
        if (!output.good()) {
            return false;
        }
        output.close();
        if (!output.good()) {
            return false;
        }

        std::error_code remove_error;
        std::filesystem::remove(final_path, remove_error);
        std::error_code rename_error;
        std::filesystem::rename(temp_path, final_path, rename_error);
        return !rename_error;
    }

    bool RuntimePersistence::Load(RuntimeState &runtime_state) const {
        std::ifstream input(StateFilePath());
        if (!input.is_open()) {
            return true;
        }

        runtime_state.databases.clear();
        std::string line;
        bool format_checked = false;
        while (std::getline(input, line)) {
            if (line.empty()) {
                continue;
            }
            const auto parts = SplitByTab(line);
            if (parts.empty()) {
                continue;
            }

            if (!format_checked) {
                format_checked = true;
                if (parts[0] == kFormatTag) {
                    if (parts.size() != 2 || parts[1] != kFormatVersion) {
                        return false;
                    }
                    continue;
                }
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
                int encoded_type = 0;
                int encoded_constraint = 0;
                if (!ParseInteger(parts[4], encoded_type) ||
                    !ParseInteger(parts[5], encoded_constraint)) {
                    return false;
                }
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
                column.type = static_cast<common::ValueType>(encoded_type);
                column.constraint =
                    static_cast<catalog::ColumnConstraint>(encoded_constraint);
                if (parts[6] == "1") {
                    const auto decoded_default = TryDecodeValue(parts[7]);
                    if (!decoded_default.has_value()) {
                        return false;
                    }
                    column.default_value = *decoded_default;
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
                    const auto decoded_value = TryDecodeValue(parts[index]);
                    if (!decoded_value.has_value()) {
                        return false;
                    }
                    row.values.push_back(*decoded_value);
                }
                if (row.values.size() != table_it->second.schema.columns.size()) {
                    return false;
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
                                EncodeIndexKeyForType(
                                    row.values[column_index],
                                    table_it->second.schema.columns[column_index]
                                        .type),
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
