#include "catalog/catalog.hpp"

// this file will manage metadata persistence and lookup routines.
#include <algorithm>

namespace dbms::catalog {

void Catalog::CreateDatabase(std::string database_name) {
    databases_.push_back(std::move(database_name));
}

void Catalog::DropDatabase(const std::string& database_name) {
    databases_.erase(
        std::remove(databases_.begin(), databases_.end(), database_name),
        databases_.end());
}

void Catalog::RegisterTable(TableSchema schema) {
    tables_.push_back(std::move(schema));
}

std::optional<TableSchema> Catalog::FindTable(
    const std::string& database_name,
    const std::string& table_name) const {
    for (const auto& table : tables_) {
        if (table.database_name == database_name && table.table_name == table_name) {
            return table;
        }
    }
    return std::nullopt;
}

}  // namespace dbms::catalog
