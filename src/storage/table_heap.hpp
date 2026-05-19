#pragma once

// this file defines heap-style row storage interfaces for table records.
#include <vector>

#include "catalog/schema.hpp"
#include "common/types.hpp"

namespace dbms::storage {

    class TableHeap {
      public:
        explicit TableHeap(catalog::TableSchema schema);

        [[nodiscard]] common::RowId Insert(common::RowData row);
        [[nodiscard]] std::vector<common::RowData> ScanAll() const;
        void ReplaceAll(std::vector<common::RowData> rows);

      private:
        catalog::TableSchema schema_;
        std::vector<common::RowData> rows_;
    };

} // namespace dbms::storage
