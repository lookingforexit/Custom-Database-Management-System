#include "catalog/rbac.hpp"

// this file implements permission checks for rbac authorization decisions.
#include <chrono>
#include <random>
#include <sstream>

namespace dbms::catalog {

    namespace {
        constexpr const char *kJwtSecret = "cw2026_demo_secret";
    }

    bool AccessController::RegisterAccount(const std::string &user_id,
                                           const std::string &password,
                                           const std::string &group,
                                           std::string &error_message) {
        if (user_id.empty() || password.empty()) {
            error_message = "user and password are required";
            return false;
        }
        if (accounts_.contains(user_id)) {
            error_message = "user already exists";
            return false;
        }
        AccountRecord record;
        record.user_id = user_id;
        record.salt = NewSalt();
        record.password_hash = HashPassword(record.salt, password);
        record.groups = {group.empty() ? "admin" : group};
        accounts_.emplace(user_id, std::move(record));
        return true;
    }

    std::optional<std::string> AccessController::LoginAndIssueToken(
        const std::string &user_id, const std::string &password,
        std::string &error_message) const {
        auto it = accounts_.find(user_id);
        if (it == accounts_.end()) {
            error_message = "user not found";
            return std::nullopt;
        }
        const auto password_hash = HashPassword(it->second.salt, password);
        if (password_hash != it->second.password_hash) {
            error_message = "invalid credentials";
            return std::nullopt;
        }
        const auto expires_unix = std::chrono::duration_cast<std::chrono::seconds>(
                                      std::chrono::system_clock::now().time_since_epoch())
                                      .count() +
                                  3600;
        return IssueToken(user_id, expires_unix);
    }

    std::optional<std::string>
    AccessController::ValidateTokenAndGetUser(const std::string &token) const {
        const auto first = token.find('.');
        const auto second = token.rfind('.');
        if (first == std::string::npos || second == std::string::npos ||
            first == second) {
            return std::nullopt;
        }
        const std::string user_id = token.substr(0, first);
        const std::string expires = token.substr(first + 1, second - first - 1);
        const std::string signature = token.substr(second + 1);
        if (user_id.empty() || expires.empty() || signature.empty()) {
            return std::nullopt;
        }
        if (SignTokenPayload(user_id + "." + expires) != signature) {
            return std::nullopt;
        }
        std::int64_t expires_unix = 0;
        try {
            expires_unix = std::stoll(expires);
        } catch (...) {
            return std::nullopt;
        }
        const auto now_unix = std::chrono::duration_cast<std::chrono::seconds>(
                                  std::chrono::system_clock::now().time_since_epoch())
                                  .count();
        if (now_unix > expires_unix) {
            return std::nullopt;
        }
        if (!accounts_.contains(user_id)) {
            return std::nullopt;
        }
        return user_id;
    }

    bool AccessController::HasAccounts() const { return !accounts_.empty(); }

    bool AccessController::Authorize(const std::string &user_id,
                                     const std::string &, Permission permission) const {
        auto it = accounts_.find(user_id);
        if (it == accounts_.end()) {
            return false;
        }
        return HasPermissionInGroups(it->second.groups, permission);
    }

    std::string AccessController::HashPassword(const std::string &salt,
                                               const std::string &password) const {
        const auto value =
            std::hash<std::string>{}(salt + ":" + password + ":" + kJwtSecret);
        return std::to_string(value);
    }

    std::string AccessController::NewSalt() const {
        static thread_local std::mt19937_64 rng{std::random_device{}()};
        std::uniform_int_distribution<std::uint64_t> distribution;
        std::ostringstream out;
        out << std::hex << distribution(rng);
        return out.str();
    }

    std::string AccessController::IssueToken(const std::string &user_id,
                                             std::int64_t expires_unix) const {
        const std::string payload = user_id + "." + std::to_string(expires_unix);
        return payload + "." + SignTokenPayload(payload);
    }

    std::string
    AccessController::SignTokenPayload(const std::string &payload) const {
        return std::to_string(std::hash<std::string>{}(payload + ":" + kJwtSecret));
    }

    bool AccessController::HasPermissionInGroups(
        const std::vector<std::string> &groups, Permission permission) const {
        for (const auto &group : groups) {
            if (group == "admin") {
                return true;
            }
            if (group == "reader" && permission == Permission::kRead) {
                return true;
            }
            if (group == "writer" &&
                (permission == Permission::kRead || permission == Permission::kWrite)) {
                return true;
            }
        }
        return false;
    }

} // namespace dbms::catalog
