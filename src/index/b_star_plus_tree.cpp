#include "index/b_star_plus_tree.hpp"

// this file will later contain the real page-based b*+-tree algorithms.
#include <algorithm>

namespace dbms::index {

void BStarPlusTree::Insert(const std::string& key, common::RowId row_id) {
    entries_[key].push_back(row_id);
}

void BStarPlusTree::Remove(const std::string& key, common::RowId row_id) {
    auto it = entries_.find(key);
    if (it == entries_.end()) {
        return;
    }

    auto& ids = it->second;
    ids.erase(std::remove(ids.begin(), ids.end(), row_id), ids.end());
    if (ids.empty()) {
        entries_.erase(it);
    }
}

std::vector<common::RowId> BStarPlusTree::Find(const std::string& key) const {
    auto it = entries_.find(key);
    if (it == entries_.end()) {
        return {};
    }
    return it->second;
}

std::vector<IndexEntry> BStarPlusTree::RangeScan(
    const std::string& lower_bound,
    const std::string& upper_bound) const {
    std::vector<IndexEntry> result;
    for (auto it = entries_.lower_bound(lower_bound);
         it != entries_.end() && it->first < upper_bound;
         ++it) {
        result.push_back(IndexEntry{it->first, it->second});
    }
    return result;
}

}  // namespace dbms::index
