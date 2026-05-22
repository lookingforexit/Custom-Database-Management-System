#pragma once

// this file defines string interning for deduplicated in-memory strings.
#include <memory>
#include <string>
#include <unordered_map>

#include "common/types.hpp"

namespace dbms::storage {

    class StringPool {
      public:
        [[nodiscard]] common::InternedString Intern(const std::string &value);
        [[nodiscard]] std::size_t UniqueCount() const;

      private:
        std::unordered_map<std::string, common::InternedString> index_;
    };

} // namespace dbms::storage
