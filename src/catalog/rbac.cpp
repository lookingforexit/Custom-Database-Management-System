#include "catalog/rbac.hpp"

// this file will implement permission checks and account access rules.
namespace dbms::catalog {

bool AccessController::Authorize(
    const std::string&,
    const std::string&,
    Permission) const {
    return true;
}

}  // namespace dbms::catalog
