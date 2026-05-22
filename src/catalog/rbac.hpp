#pragma once

// this file defines persistent rbac/auth models for users, groups, grants,
// and jwt-backed authorization.
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
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

    struct DatabaseGrantRecord {
        std::uint32_t default_permissions{0};
        std::unordered_map<std::string, std::uint32_t> group_permissions;
        std::unordered_map<std::string, std::uint32_t> user_permissions;
    };

    class AccessController {
      public:
        explicit AccessController(std::string root_path);

        [[nodiscard]] bool RegisterAccount(const std::string &user_id,
                                           const std::string &password,
                                           const std::string &group,
                                           std::string &error_message);
        [[nodiscard]] bool CreateGroup(const std::string &group_name,
                                       std::string &error_message);
        [[nodiscard]] bool AddUserToGroup(const std::string &user_id,
                                          const std::string &group_name,
                                          std::string &error_message);
        [[nodiscard]] bool GrantDefault(const std::string &database_name,
                                        Permission permission,
                                        std::string &error_message);
        [[nodiscard]] bool RevokeDefault(const std::string &database_name,
                                         Permission permission,
                                         std::string &error_message);
        [[nodiscard]] bool GrantGroup(const std::string &database_name,
                                      const std::string &group_name,
                                      Permission permission,
                                      std::string &error_message);
        [[nodiscard]] bool RevokeGroup(const std::string &database_name,
                                       const std::string &group_name,
                                       Permission permission,
                                       std::string &error_message);
        [[nodiscard]] bool GrantUser(const std::string &database_name,
                                     const std::string &user_id,
                                     Permission permission,
                                     std::string &error_message);
        [[nodiscard]] bool RevokeUser(const std::string &database_name,
                                      const std::string &user_id,
                                      Permission permission,
                                      std::string &error_message);
        [[nodiscard]] std::optional<std::string>
        LoginAndIssueToken(const std::string &user_id,
                           const std::string &password,
                           std::string &error_message) const;
        [[nodiscard]] std::optional<std::string>
        ValidateTokenAndGetUser(const std::string &token) const;
        [[nodiscard]] bool HasAccounts() const;
        [[nodiscard]] bool IsAdmin(const std::string &user_id) const;
        [[nodiscard]] bool Authorize(const std::string &user_id,
                                     const std::string &database_name,
                                     Permission permission) const;
        [[nodiscard]] std::vector<std::string>
        UserGroups(const std::string &user_id) const;
        [[nodiscard]] static std::optional<Permission>
        ParsePermissionName(const std::string &permission_name);

      private:
        [[nodiscard]] bool Persist(std::string &error_message) const;
        [[nodiscard]] bool Load();
        [[nodiscard]] bool EnsureJwtSecret();
        [[nodiscard]] static std::string AccountsFilePath(const std::string &root_path);
        [[nodiscard]] static std::string GrantsFilePath(const std::string &root_path);
        [[nodiscard]] static std::string SecretFilePath(const std::string &root_path);
        [[nodiscard]] std::string HashPassword(const std::string &salt,
                                               const std::string &password) const;
        [[nodiscard]] std::string NewSalt() const;
        [[nodiscard]] std::string NewSecret() const;
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
        [[nodiscard]] bool HasPermissionInMask(std::uint32_t mask,
                                               Permission permission) const;
        [[nodiscard]] static std::uint32_t PermissionMask(Permission permission);
        std::string root_path_;
        std::string jwt_secret_;
        std::unordered_map<std::string, AccountRecord> accounts_;
        std::unordered_set<std::string> groups_;
        std::unordered_map<std::string, DatabaseGrantRecord> database_grants_;
    };

} // namespace dbms::catalog
