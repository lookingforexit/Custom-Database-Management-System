#pragma once

// this file defines metadata catalog interfaces for databases, tables, and
// schemas.
#include <optional>
#include <string>
#include <vector>

#include "catalog/schema.hpp"

namespace dbms::catalog {

    class Catalog {
      public:
        explicit Catalog(std::string root_path);

        [[nodiscard]] const std::string &root_path() const;

        void CreateDatabase(std::string database_name);
        void DropDatabase(const std::string &database_name);
        void RegisterTable(TableSchema schema);
        [[nodiscard]] std::optional<TableSchema>
        FindTable(const std::string &database_name,
                  const std::string &table_name) const;

      private:
        std::string root_path_;
        std::vector<std::string> databases_;
        std::vector<TableSchema> tables_;
    };

} // namespace dbms::catalog
