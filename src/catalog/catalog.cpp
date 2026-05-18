#include "catalog/catalog.hpp"

// this file implements metadata catalog storage and lookup routines.
#include <algorithm>

namespace dbms::catalog {

    Catalog::Catalog(std::string root_path)
        : root_path_(std::move(root_path)) {}

    const std::string &Catalog::root_path() const { return root_path_; }

    void Catalog::CreateDatabase(std::string database_name) {
        databases_.push_back(std::move(database_name));
    }

    void Catalog::DropDatabase(const std::string &database_name) {
        databases_.erase(
            std::remove(databases_.begin(), databases_.end(), database_name),
            databases_.end());
    }

    void Catalog::RegisterTable(TableSchema schema) {
        tables_.push_back(std::move(schema));
    }

    std::optional<TableSchema>
    Catalog::FindTable(const std::string &database_name,
                       const std::string &table_name) const {
        for (const auto &table : tables_) {
            if (table.database_name == database_name &&
                table.table_name == table_name) {
                return table;
            }
        }
        return std::nullopt;
    }

} // namespace dbms::catalog
