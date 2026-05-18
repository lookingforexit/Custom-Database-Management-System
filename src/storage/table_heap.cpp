#include "storage/table_heap.hpp"

// this file will implement row insert, scan, and update storage behavior.
namespace dbms::storage {

TableHeap::TableHeap(catalog::TableSchema schema)
    : schema_(std::move(schema)) {}

common::RowId TableHeap::Insert(common::Row row) {
    rows_.push_back(std::move(row));
    return rows_.size() - 1;
}

std::vector<common::Row> TableHeap::ScanAll() const {
    return rows_;
}

}  // namespace dbms::storage
