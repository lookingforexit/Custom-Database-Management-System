#include "storage/page_manager.hpp"

// this file will manage page files and on-disk allocation strategy.
namespace dbms::storage {

PageManager::PageManager(std::string root_directory)
    : root_directory_(std::move(root_directory)) {}

PageId PageManager::AllocatePage() {
    return next_page_id_++;
}

}  // namespace dbms::storage
