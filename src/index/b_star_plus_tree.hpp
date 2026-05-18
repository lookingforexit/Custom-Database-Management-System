#pragma once

// this file defines the main b*+-tree index abstraction used by indexed
// columns.
#include <map>
#include <string>
#include <vector>

#include "common/types.hpp"

namespace dbms::index {

  struct IndexEntry {
    std::string key;
    std::vector<common::RowId> row_ids;
  };

  class BStarPlusTree {
  public:
    BStarPlusTree() = default;

    void Insert(const std::string &key, common::RowId row_id);
    void Remove(const std::string &key, common::RowId row_id);
    [[nodiscard]] std::vector<common::RowId> Find(const std::string &key) const;
    [[nodiscard]] std::vector<IndexEntry>
    RangeScan(const std::string &lower_bound,
              const std::string &upper_bound) const;

  private:
    // Temporary in-memory placeholder until the real page-based B*+-tree lands.
    std::map<std::string, std::vector<common::RowId>> entries_;
  };

} // namespace dbms::index
