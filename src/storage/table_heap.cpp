#include "storage/table_heap.hpp"

// this file implements row insertion and full-table scan behavior for table
// heaps.
namespace dbms::storage {

    TableHeap::TableHeap(catalog::TableSchema schema)
        : schema_(std::move(schema)) {}

    common::RowId TableHeap::Insert(common::RowData row) {
        if (row.row_id == common::kInvalidRowId) {
            row.row_id = next_row_id_++;
        } else if (row.row_id >= next_row_id_) {
            next_row_id_ = row.row_id + 1;
        }
        rows_.push_back(std::move(row));
        return rows_.back().row_id;
    }

    std::vector<common::RowData> TableHeap::ScanAll() const { return rows_; }

    std::optional<common::RowData>
    TableHeap::FindByRowId(common::RowId row_id) const {
        for (const auto &row : rows_) {
            if (row.row_id == row_id) {
                return row;
            }
        }
        return std::nullopt;
    }

    void TableHeap::ReplaceAll(std::vector<common::RowData> rows) {
        rows_ = std::move(rows);
        next_row_id_ = 0;
        for (const auto &row : rows_) {
            if (row.row_id != common::kInvalidRowId && row.row_id >= next_row_id_) {
                next_row_id_ = row.row_id + 1;
            }
        }
    }

} // namespace dbms::storage
