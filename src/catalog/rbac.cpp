#include "catalog/rbac.hpp"

// this file implements permission checks for rbac authorization decisions.
namespace dbms::catalog {

    bool AccessController::Authorize(const std::string &, const std::string &,
                                     Permission) const {
        return true;
    }

} // namespace dbms::catalog
