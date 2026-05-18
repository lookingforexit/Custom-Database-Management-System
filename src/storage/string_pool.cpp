#include "storage/string_pool.hpp"

// this file will store one copy of each unique string value.
namespace dbms::storage {

StringId StringPool::Intern(const std::string& value) {
    auto it = index_.find(value);
    if (it != index_.end()) {
        return it->second;
    }

    const StringId id = values_.size();
    values_.push_back(value);
    index_.emplace(values_.back(), id);
    return id;
}

const std::string& StringPool::Resolve(StringId id) const {
    return values_.at(id);
}

}  // namespace dbms::storage
