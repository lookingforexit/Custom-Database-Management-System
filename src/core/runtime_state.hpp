#pragma once

// this file defines in-memory runtime objects for databases, tables, heaps, and
// indexes.

#include <memory>
#include <string>
#include <unordered_map>

#include "index/b_star_plus_tree.hpp"
#include "storage/table_heap.hpp"

namespace dbms::core {

  struct TableRuntime {
    catalog::TableSchema schema;
    std::unique_ptr<storage::TableHeap> heap;
    std::unordered_map<std::string, std::unique_ptr<index::BStarPlusTree>>
        indexes;
  };

  struct DatabaseRuntime {
    std::string name;
    std::unordered_map<std::string, TableRuntime> tables;
  };

  struct RuntimeState {
    std::unordered_map<std::string, DatabaseRuntime> databases;
  };

} // namespace dbms::core
