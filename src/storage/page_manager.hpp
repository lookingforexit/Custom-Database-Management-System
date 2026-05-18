#pragma once

// this file declares page allocation and low-level storage primitives.
#include <cstddef>
#include <cstdint>
#include <string>

namespace dbms::storage {

using PageId = std::uint64_t;

class PageManager {
public:
    explicit PageManager(std::string root_directory);

    [[nodiscard]] PageId AllocatePage();
    [[nodiscard]] std::size_t page_size() const { return page_size_; }

private:
    std::string root_directory_;
    std::size_t page_size_{4096};
    PageId next_page_id_{1};
};

}  // namespace dbms::storage
