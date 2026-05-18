#pragma once

// this file declares heap-style row storage for table records.
#include <string>
#include <vector>

#include "catalog/schema.hpp"
#include "common/types.hpp"

namespace dbms::storage {

class TableHeap {
public:
    explicit TableHeap(catalog::TableSchema schema);

    [[nodiscard]] common::RowId Insert(common::RowData row);
    [[nodiscard]] std::vector<common::RowData> ScanAll() const;

private:
    catalog::TableSchema schema_;
    std::vector<common::RowData> rows_;
};

}  // namespace dbms::storage
