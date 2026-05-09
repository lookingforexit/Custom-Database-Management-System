#pragma once

// this file declares authz models for users, groups, and permissions.
#include <cstdint>
#include <string>
#include <vector>

namespace dbms::catalog {

enum class Permission : std::uint32_t {
    kRead = 1U << 0U,
    kWrite = 1U << 1U,
    kCreateTable = 1U << 2U,
    kDropTable = 1U << 3U,
    kDropDatabase = 1U << 4U,
};

struct AccountRecord {
    std::string user_id;
    std::string salt;
    std::string password_hash;
    std::vector<std::string> groups;
};

class AccessController {
public:
    [[nodiscard]] bool Authorize(
        const std::string& user_id,
        const std::string& database_name,
        Permission permission) const;
};

}  // namespace dbms::catalog
