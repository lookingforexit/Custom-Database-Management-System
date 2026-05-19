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
        constexpr std::string_view kFormatVersion = "2";

        bool IsSupportedFormatVersion(const std::string &version) {
            return version == "1" || version == "2";
        }

        bool ParseKeyValueToken(const std::string &token, std::string &key,
                                std::string &value) {
            const auto position = token.find('=');
            if (position == std::string::npos || position == 0 ||
                position + 1 >= token.size()) {
                return false;
            }
            key = token.substr(0, position);
            value = token.substr(position + 1);
            return true;
        }

        std::optional<std::string>
        FindFieldValue(const std::vector<std::string> &parts,
                       const std::string &field_name) {
            for (std::size_t index = 1; index < parts.size(); ++index) {
                std::string key;
                std::string value;
                if (!ParseKeyValueToken(parts[index], key, value)) {
                    continue;
                }
                if (key == field_name) {
                    return value;
                }
            }
            return std::nullopt;
        }

        std::string ValueTypeName(common::ValueType type) {
            switch (type) {
                case common::ValueType::kNull:
                    return "NULL";
                case common::ValueType::kInt64:
                    return "INT";
                case common::ValueType::kString:
                    return "STRING";
            }
            return "UNKNOWN";
        }

        std::string ConstraintName(catalog::ColumnConstraint constraint) {
            switch (constraint) {
                case catalog::ColumnConstraint::kNone:
                    return "NONE";
                case catalog::ColumnConstraint::kNotNull:
                    return "NOT_NULL";
                case catalog::ColumnConstraint::kIndexed:
                    return "INDEXED";
            }
            return "UNKNOWN";
        }

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
                return "Null";
            }
            if (std::holds_alternative<std::int64_t>(value)) {
                return "Int:" + std::to_string(std::get<std::int64_t>(value));
            }
            return "String:" + Escape(std::get<std::string>(value));
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
            if (encoded == "N" || encoded == "Null") {
                return std::monostate{};
            }
            if (encoded.rfind("I:", 0) == 0 ||
                encoded.rfind("Int:", 0) == 0) {
                try {
                    const std::size_t prefix_len =
                        encoded.rfind("Int:", 0) == 0 ? 4 : 2;
                    std::size_t consumed = 0;
                    const auto parsed =
                        std::stoll(encoded.substr(prefix_len), &consumed);
                    if (consumed != encoded.size() - prefix_len) {
                        return std::nullopt;
                    }
                    return static_cast<std::int64_t>(parsed);
                } catch (...) {
                    return std::nullopt;
                }
            }
            if (encoded.rfind("S:", 0) == 0 ||
                encoded.rfind("String:", 0) == 0) {
                const std::size_t prefix_len =
                    encoded.rfind("String:", 0) == 0 ? 7 : 2;
                return Unescape(encoded.substr(prefix_len));
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

    std::string RuntimePersistence::HistoryFilePath() const {
        return root_path_ + "/version_history.tsv";
    }

    bool RuntimePersistence::Save(
        const RuntimeState &runtime_state,
        const versioning::VersionStore &version_store) const {
        std::filesystem::create_directories(root_path_);
        const auto final_path = StateFilePath();
        const auto temp_path = final_path + ".tmp";
        std::ofstream output(temp_path, std::ios::trunc);
        if (!output.is_open()) {
            return false;
        }

        output << kFormatTag << "\t" << kFormatVersion << "\n";
        output << "# Runtime state dump (human-readable)\n";
        output << "# Records:\n";
        output << "# DATABASE\\tname=<db>\n";
        output << "# TABLE\\tdb=<db>\\tname=<table>\n";
        output << "# COLUMN\\tdb=<db>\\ttable=<table>\\tname=<column>\\ttype=<INT|STRING|NULL>\\tconstraint=<NONE|NOT_NULL|INDEXED>\\tdefault=<value>\n";
        output << "# ROW\\tdb=<db>\\ttable=<table>\\tvalue=<...>\\tvalue=<...>\n";
        for (const auto &[database_name, database_runtime] :
             runtime_state.databases) {
            output << "DATABASE\tname=" << Escape(database_name) << "\n";
            for (const auto &[table_name, table_runtime] : database_runtime.tables) {
                output << "# Database: " << Escape(database_name) << "\n";
                output << "# Table: " << Escape(table_name) << "\n";
                output << "TABLE\tdb=" << Escape(database_name) << "\tname="
                       << Escape(table_name) << "\n";
                for (const auto &column : table_runtime.schema.columns) {
                    output << "COLUMN\tdb=" << Escape(database_name)
                           << "\ttable=" << Escape(table_name)
                           << "\tname=" << Escape(column.name)
                           << "\ttype=" << ValueTypeName(column.type)
                           << "\tconstraint="
                           << ConstraintName(column.constraint) << "\tdefault="
                           << (column.default_value.has_value()
                                   ? EncodeValue(*column.default_value)
                                   : "Null")
                           << "\n";
                }

                const auto rows = table_runtime.heap->ScanAll();
                for (const auto &row : rows) {
                    output << "ROW\tdb=" << Escape(database_name) << "\ttable="
                           << Escape(table_name);
                    for (const auto &value : row.values) {
                        output << "\tvalue=" << EncodeValue(value);
                    }
                    output << "\n";
                }
                output << "\n";
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
        if (rename_error) {
            return false;
        }

        const auto history_final_path = HistoryFilePath();
        const auto history_temp_path = history_final_path + ".tmp";
        std::ofstream history_output(history_temp_path, std::ios::trunc);
        if (!history_output.is_open()) {
            return false;
        }
        history_output << kFormatTag << "\t" << kFormatVersion << "\n";
        history_output << "# Version history dump (human-readable)\n";
        history_output << "# Records:\n";
        history_output << "# CHANGE\\tdb=<db>\\ttable=<table>\\toperation=<op>\\ttimestamp=<ts>\n";
        history_output << "# SNAPSHOT_ROW\\tvalue=<...>\\tvalue=<...>\n";
        history_output << "# CHANGE_END\n";
        for (const auto &record : version_store.AllRecords()) {
            history_output << "CHANGE\tdb=" << Escape(record.database_name)
                           << "\ttable=" << Escape(record.table_name)
                           << "\toperation=" << Escape(record.operation)
                           << "\ttimestamp=" << Escape(record.timestamp) << "\n";
            for (const auto &row : record.snapshot_rows) {
                history_output << "SNAPSHOT_ROW";
                for (const auto &value : row.values) {
                    history_output << "\tvalue=" << EncodeValue(value);
                }
                history_output << "\n";
            }
            history_output << "CHANGE_END\n";
            history_output << "\n";
        }
        history_output.flush();
        if (!history_output.good()) {
            return false;
        }
        history_output.close();
        if (!history_output.good()) {
            return false;
        }

        std::error_code history_remove_error;
        std::filesystem::remove(history_final_path, history_remove_error);
        std::error_code history_rename_error;
        std::filesystem::rename(history_temp_path, history_final_path,
                                history_rename_error);
        return !history_rename_error;
    }

    bool RuntimePersistence::Load(
        RuntimeState &runtime_state,
        versioning::VersionStore &version_store) const {
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
            if (line.front() == '#') {
                continue;
            }
            const auto parts = SplitByTab(line);
            if (parts.empty()) {
                continue;
            }

            if (!format_checked) {
                format_checked = true;
                if (parts[0] == kFormatTag) {
                    if (parts.size() != 2 ||
                        !IsSupportedFormatVersion(parts[1])) {
                        return false;
                    }
                    continue;
                }
            }

            if (parts[0] == "DATABASE") {
                const auto database_name_field = FindFieldValue(parts, "name");
                if (!database_name_field.has_value()) {
                    return false;
                }
                const std::string database_name = Unescape(*database_name_field);
                runtime_state.databases.emplace(
                    database_name,
                    DatabaseRuntime{.name = database_name, .tables = {}});
                continue;
            }

            if (parts[0] == "DB" && parts.size() >= 2) {
                const std::string database_name = Unescape(parts[1]);
                runtime_state.databases.emplace(
                    database_name,
                    DatabaseRuntime{.name = database_name, .tables = {}});
                continue;
            }

            if (parts[0] == "TABLE") {
                std::optional<std::string> database_name_field =
                    FindFieldValue(parts, "db");
                std::optional<std::string> table_name_field =
                    FindFieldValue(parts, "name");
                std::string database_name;
                std::string table_name;
                if (database_name_field.has_value() &&
                    table_name_field.has_value()) {
                    database_name = Unescape(*database_name_field);
                    table_name = Unescape(*table_name_field);
                } else if (parts.size() >= 3) {
                    database_name = Unescape(parts[1]);
                    table_name = Unescape(parts[2]);
                } else {
                    return false;
                }
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

            if (parts[0] == "COLUMN") {
                std::string database_name;
                std::string table_name;
                std::string column_name;
                int encoded_type = 0;
                int encoded_constraint = 0;
                std::optional<common::Value> parsed_default = std::nullopt;

                const auto db_field = FindFieldValue(parts, "db");
                const auto table_field = FindFieldValue(parts, "table");
                const auto name_field = FindFieldValue(parts, "name");
                const auto type_field = FindFieldValue(parts, "type");
                const auto constraint_field = FindFieldValue(parts, "constraint");
                const auto default_field = FindFieldValue(parts, "default");
                if (db_field.has_value() && table_field.has_value() &&
                    name_field.has_value() && type_field.has_value() &&
                    constraint_field.has_value() && default_field.has_value()) {
                    database_name = Unescape(*db_field);
                    table_name = Unescape(*table_field);
                    column_name = Unescape(*name_field);

                    const auto type_upper = *type_field;
                    if (type_upper == "INT") {
                        encoded_type = static_cast<int>(common::ValueType::kInt64);
                    } else if (type_upper == "STRING") {
                        encoded_type = static_cast<int>(common::ValueType::kString);
                    } else if (type_upper == "NULL") {
                        encoded_type = static_cast<int>(common::ValueType::kNull);
                    } else {
                        return false;
                    }

                    const auto constraint_upper = *constraint_field;
                    if (constraint_upper == "NONE") {
                        encoded_constraint =
                            static_cast<int>(catalog::ColumnConstraint::kNone);
                    } else if (constraint_upper == "NOT_NULL") {
                        encoded_constraint = static_cast<int>(
                            catalog::ColumnConstraint::kNotNull);
                    } else if (constraint_upper == "INDEXED") {
                        encoded_constraint = static_cast<int>(
                            catalog::ColumnConstraint::kIndexed);
                    } else {
                        return false;
                    }

                    const auto decoded_default = TryDecodeValue(*default_field);
                    if (!decoded_default.has_value()) {
                        return false;
                    }
                    if (!std::holds_alternative<std::monostate>(*decoded_default)) {
                        parsed_default = *decoded_default;
                    }
                } else if (parts.size() >= 8) {
                    database_name = Unescape(parts[1]);
                    table_name = Unescape(parts[2]);
                    column_name = Unescape(parts[3]);
                    if (!ParseInteger(parts[4], encoded_type) ||
                        !ParseInteger(parts[5], encoded_constraint)) {
                        return false;
                    }
                    if (parts[6] == "1") {
                        const auto decoded_default = TryDecodeValue(parts[7]);
                        if (!decoded_default.has_value()) {
                            return false;
                        }
                        parsed_default = *decoded_default;
                    }
                } else {
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
                if (parsed_default.has_value()) {
                    column.default_value = *parsed_default;
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

            if (parts[0] == "ROW") {
                std::string database_name;
                std::string table_name;
                std::size_t value_start_index = 3;
                const auto db_field = FindFieldValue(parts, "db");
                const auto table_field = FindFieldValue(parts, "table");
                if (db_field.has_value() && table_field.has_value()) {
                    database_name = Unescape(*db_field);
                    table_name = Unescape(*table_field);
                    value_start_index = 0;
                } else if (parts.size() >= 3) {
                    database_name = Unescape(parts[1]);
                    table_name = Unescape(parts[2]);
                } else {
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

                common::RowData row;
                for (std::size_t index = value_start_index; index < parts.size();
                     ++index) {
                    std::string encoded_value = parts[index];
                    if (value_start_index == 0) {
                        std::string key;
                        std::string value;
                        if (!ParseKeyValueToken(parts[index], key, value) ||
                            key != "value") {
                            continue;
                        }
                        encoded_value = value;
                    }
                    const auto decoded_value = TryDecodeValue(encoded_value);
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

        std::ifstream history_input(HistoryFilePath());
        if (!history_input.is_open()) {
            version_store.Clear();
            return true;
        }
        std::vector<versioning::ChangeRecord> loaded_records;
        std::optional<versioning::ChangeRecord> pending_record;
        format_checked = false;
        while (std::getline(history_input, line)) {
            if (line.empty()) {
                continue;
            }
            if (line.front() == '#') {
                continue;
            }
            const auto parts = SplitByTab(line);
            if (parts.empty()) {
                continue;
            }

            if (!format_checked) {
                format_checked = true;
                if (parts[0] == kFormatTag) {
                    if (parts.size() != 2 ||
                        !IsSupportedFormatVersion(parts[1])) {
                        return false;
                    }
                    continue;
                }
            }

            if (parts[0] == "CHANGE") {
                if (pending_record.has_value()) {
                    return false;
                }
                const auto db_field = FindFieldValue(parts, "db");
                const auto table_field = FindFieldValue(parts, "table");
                const auto operation_field = FindFieldValue(parts, "operation");
                const auto timestamp_field = FindFieldValue(parts, "timestamp");
                if (db_field.has_value() && table_field.has_value() &&
                    operation_field.has_value() && timestamp_field.has_value()) {
                    pending_record = versioning::ChangeRecord{
                        .database_name = Unescape(*db_field),
                        .table_name = Unescape(*table_field),
                        .operation = Unescape(*operation_field),
                        .timestamp = Unescape(*timestamp_field),
                        .snapshot_rows = {},
                    };
                    continue;
                }
                if (parts.size() == 5) {
                    pending_record = versioning::ChangeRecord{
                        .database_name = Unescape(parts[1]),
                        .table_name = Unescape(parts[2]),
                        .operation = Unescape(parts[3]),
                        .timestamp = Unescape(parts[4]),
                        .snapshot_rows = {},
                    };
                    continue;
                }
                return false;
            }
            if (parts[0] == "SNAPSHOT_ROW") {
                if (!pending_record.has_value()) {
                    return false;
                }
                common::RowData row;
                for (std::size_t index = 1; index < parts.size(); ++index) {
                    std::string encoded_value = parts[index];
                    std::string key;
                    std::string value;
                    if (ParseKeyValueToken(parts[index], key, value) &&
                        key == "value") {
                        encoded_value = value;
                    }
                    const auto decoded_value = TryDecodeValue(encoded_value);
                    if (!decoded_value.has_value()) {
                        return false;
                    }
                    row.values.push_back(*decoded_value);
                }
                pending_record->snapshot_rows.push_back(std::move(row));
                continue;
            }
            if (parts[0] == "CHANGE_END" && parts.size() == 1) {
                if (!pending_record.has_value()) {
                    return false;
                }
                loaded_records.push_back(std::move(*pending_record));
                pending_record.reset();
                continue;
            }
            return false;
        }
        if (pending_record.has_value()) {
            return false;
        }
        version_store.ReplaceAll(std::move(loaded_records));
        auto index_validation = ValidateRuntimeIndexConsistency(runtime_state);
        if (!index_validation.ok()) {
            return false;
        }
        return true;
    }

} // namespace dbms::core
