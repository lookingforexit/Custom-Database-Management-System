#include "storage/page_manager.hpp"

// this file implements page file allocation and low-level storage behavior.
namespace dbms::storage {

  PageManager::PageManager(std::string root_directory)
      : root_directory_(std::move(root_directory)) {}

  PageId PageManager::AllocatePage() { return next_page_id_++; }

} // namespace dbms::storage
