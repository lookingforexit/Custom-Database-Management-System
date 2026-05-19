#pragma once

// this file defines string interning for deduplicated in-memory strings.
#include <string>
#include <unordered_map>
#include <vector>

namespace dbms::storage {

    using StringId = std::size_t;

    class StringPool {
      public:
        [[nodiscard]] StringId Intern(const std::string &value);
        [[nodiscard]] const std::string &Resolve(StringId id) const;
        [[nodiscard]] std::size_t UniqueCount() const;

      private:
        std::unordered_map<std::string, StringId> index_;
        std::vector<std::string> values_;
    };

} // namespace dbms::storage
