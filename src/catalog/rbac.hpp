#pragma once

// this file defines rbac models for users, groups, and permission checks.
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
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
        [[nodiscard]] bool RegisterAccount(const std::string &user_id,
                                           const std::string &password,
                                           const std::string &group,
                                           std::string &error_message);
        [[nodiscard]] std::optional<std::string>
        LoginAndIssueToken(const std::string &user_id,
                           const std::string &password,
                           std::string &error_message) const;
        [[nodiscard]] std::optional<std::string>
        ValidateTokenAndGetUser(const std::string &token) const;
        [[nodiscard]] bool HasAccounts() const;
        [[nodiscard]] bool Authorize(const std::string &user_id,
                                     const std::string &database_name,
                                     Permission permission) const;

      private:
        [[nodiscard]] std::string HashPassword(const std::string &salt,
                                               const std::string &password) const;
        [[nodiscard]] std::string NewSalt() const;
        [[nodiscard]] std::string
        IssueToken(const std::string &user_id, std::int64_t expires_unix) const;
        [[nodiscard]] std::string
        SignTokenPayload(const std::string &payload) const;
        [[nodiscard]] std::string Base64UrlEncode(const std::string &data) const;
        [[nodiscard]] std::optional<std::string>
        Base64UrlDecode(const std::string &data) const;
        [[nodiscard]] std::optional<std::int64_t>
        ExtractNumericClaim(const std::string &json, const std::string &key) const;
        [[nodiscard]] std::optional<std::string>
        ExtractStringClaim(const std::string &json, const std::string &key) const;
        [[nodiscard]] bool HasPermissionInGroups(
            const std::vector<std::string> &groups, Permission permission) const;

        std::unordered_map<std::string, AccountRecord> accounts_;
    };

} // namespace dbms::catalog
