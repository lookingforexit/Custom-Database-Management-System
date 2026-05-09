#pragma once

// this file declares the metadata catalog for databases and tables.
#include <optional>
#include <string>
#include <vector>

#include "catalog/schema.hpp"

namespace dbms::catalog {

class Catalog {
public:
    Catalog() = default;

    void CreateDatabase(std::string database_name);
    void DropDatabase(const std::string& database_name);
    void RegisterTable(TableSchema schema);
    [[nodiscard]] std::optional<TableSchema> FindTable(
        const std::string& database_name,
        const std::string& table_name) const;

private:
    std::vector<std::string> databases_;
    std::vector<TableSchema> tables_;
};

}  // namespace dbms::catalog
