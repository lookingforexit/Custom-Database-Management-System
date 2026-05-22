#pragma once

// this file defines heap-style row storage interfaces for table records.
#include <optional>
#include <vector>

#include "catalog/schema.hpp"
#include "common/types.hpp"

namespace dbms::storage {

    class TableHeap {
      public:
        explicit TableHeap(catalog::TableSchema schema);

        [[nodiscard]] common::RowId Insert(common::RowData row);
        [[nodiscard]] std::vector<common::RowData> ScanAll() const;
        [[nodiscard]] std::optional<common::RowData> FindByRowId(common::RowId row_id) const;
        void ReplaceAll(std::vector<common::RowData> rows);

      private:
        catalog::TableSchema schema_;
        std::vector<common::RowData> rows_;
        common::RowId next_row_id_{0};
    };

} // namespace dbms::storage
