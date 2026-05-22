#include "storage/string_pool.hpp"

// this file implements string interning by storing one copy of each unique
// value.
namespace dbms::storage {

    common::InternedString StringPool::Intern(const std::string &value) {
        auto it = index_.find(value);
        if (it != index_.end()) {
            return it->second;
        }

        auto owned = std::make_shared<const std::string>(value);
        index_.emplace(*owned, owned);
        return owned;
    }

    std::size_t StringPool::UniqueCount() const { return index_.size(); }

} // namespace dbms::storage
