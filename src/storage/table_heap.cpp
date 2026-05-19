#include "storage/table_heap.hpp"

// this file implements row insertion and full-table scan behavior for table
// heaps.
namespace dbms::storage {

    TableHeap::TableHeap(catalog::TableSchema schema)
        : schema_(std::move(schema)) {}

    common::RowId TableHeap::Insert(common::RowData row) {
        rows_.push_back(std::move(row));
        return rows_.size() - 1;
    }

    std::vector<common::RowData> TableHeap::ScanAll() const { return rows_; }

    void TableHeap::ReplaceAll(std::vector<common::RowData> rows) {
        rows_ = std::move(rows);
    }

} // namespace dbms::storage
