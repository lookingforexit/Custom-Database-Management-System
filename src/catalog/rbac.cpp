#include "catalog/rbac.hpp"

// this file implements persistent rbac with per-database default/group/user
// grants and jwt-based authentication.
#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <random>
#include <sstream>
#include <vector>

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

namespace dbms::catalog {

    namespace {
        constexpr const char *kJwtHeaderJson = R"({"alg":"HS256","typ":"JWT"})";

        constexpr char kBase64UrlAlphabet[] =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

        std::string EscapeField(const std::string &value) {
            std::string escaped;
            escaped.reserve(value.size());
            for (char ch : value) {
                if (ch == '\\' || ch == '\t' || ch == '\n') {
                    escaped.push_back('\\');
                }
                escaped.push_back(ch);
            }
            return escaped;
        }

        std::string UnescapeField(const std::string &value) {
            std::string unescaped;
            unescaped.reserve(value.size());
            bool escaped = false;
            for (char ch : value) {
                if (escaped) {
                    unescaped.push_back(ch);
                    escaped = false;
                } else if (ch == '\\') {
                    escaped = true;
                } else {
                    unescaped.push_back(ch);
                }
            }
            return unescaped;
        }

        std::vector<std::string> SplitByTab(const std::string &line) {
            std::vector<std::string> parts;
            std::string current;
            bool escaped = false;
            for (char ch : line) {
                if (escaped) {
                    current.push_back(ch);
                    escaped = false;
                    continue;
                }
                if (ch == '\\') {
                    escaped = true;
                    current.push_back(ch);
                    continue;
                }
                if (ch == '\t') {
                    parts.push_back(current);
                    current.clear();
                    continue;
                }
                current.push_back(ch);
            }
            parts.push_back(current);
            return parts;
        }

        std::string JoinGroups(const std::vector<std::string> &groups) {
            std::ostringstream output;
            for (std::size_t index = 0; index < groups.size(); ++index) {
                if (index != 0) {
                    output << ",";
                }
                output << EscapeField(groups[index]);
            }
            return output.str();
        }

        std::vector<std::string> SplitGroups(const std::string &raw) {
            std::vector<std::string> groups;
            std::string current;
            bool escaped = false;
            for (char ch : raw) {
                if (escaped) {
                    current.push_back(ch);
                    escaped = false;
                    continue;
                }
                if (ch == '\\') {
                    escaped = true;
                    current.push_back(ch);
                    continue;
                }
                if (ch == ',') {
                    if (!current.empty()) {
                        groups.push_back(UnescapeField(current));
                    }
                    current.clear();
                    continue;
                }
                current.push_back(ch);
            }
            if (!current.empty()) {
                groups.push_back(UnescapeField(current));
            }
            return groups;
        }

        std::string ToUpper(std::string value) {
            for (char &ch : value) {
                ch = static_cast<char>(
                    std::toupper(static_cast<unsigned char>(ch)));
            }
            return value;
        }

    } // namespace

    AccessController::AccessController(std::string root_path)
        : root_path_(std::move(root_path)) {
        std::filesystem::create_directories(root_path_);
        (void)Load();
        (void)EnsureJwtSecret();
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

        const std::string effective_group =
            group.empty() ? (accounts_.empty() ? "admin" : "user") : group;
        groups_.insert(effective_group);

        AccountRecord record;
        record.user_id = user_id;
        record.salt = NewSalt();
        record.password_hash = HashPassword(record.salt, password);
        record.groups = {effective_group};
        accounts_.emplace(user_id, std::move(record));
        return Persist(error_message);
    }

    bool AccessController::CreateGroup(const std::string &group_name,
                                       std::string &error_message) {
        if (group_name.empty()) {
            error_message = "group is required";
            return false;
        }
        groups_.insert(group_name);
        return Persist(error_message);
    }

    bool AccessController::AddUserToGroup(const std::string &user_id,
                                          const std::string &group_name,
                                          std::string &error_message) {
        auto it = accounts_.find(user_id);
        if (it == accounts_.end()) {
            error_message = "user not found";
            return false;
        }
        if (!groups_.contains(group_name)) {
            error_message = "group not found";
            return false;
        }
        if (std::find(it->second.groups.begin(), it->second.groups.end(),
                      group_name) == it->second.groups.end()) {
            it->second.groups.push_back(group_name);
        }
        return Persist(error_message);
    }

    bool AccessController::GrantDefault(const std::string &database_name,
                                        Permission permission,
                                        std::string &error_message) {
        if (database_name.empty()) {
            error_message = "database is required";
            return false;
        }
        database_grants_[database_name].default_permissions |=
            PermissionMask(permission);
        return Persist(error_message);
    }

    bool AccessController::RevokeDefault(const std::string &database_name,
                                         Permission permission,
                                         std::string &error_message) {
        if (!database_grants_.contains(database_name)) {
            error_message = "database grant record not found";
            return false;
        }
        database_grants_[database_name].default_permissions &=
            ~PermissionMask(permission);
        return Persist(error_message);
    }

    bool AccessController::GrantGroup(const std::string &database_name,
                                      const std::string &group_name,
                                      Permission permission,
                                      std::string &error_message) {
        if (!groups_.contains(group_name)) {
            error_message = "group not found";
            return false;
        }
        database_grants_[database_name].group_permissions[group_name] |=
            PermissionMask(permission);
        return Persist(error_message);
    }

    bool AccessController::RevokeGroup(const std::string &database_name,
                                       const std::string &group_name,
                                       Permission permission,
                                       std::string &error_message) {
        if (!database_grants_.contains(database_name)) {
            error_message = "database grant record not found";
            return false;
        }
        database_grants_[database_name].group_permissions[group_name] &=
            ~PermissionMask(permission);
        return Persist(error_message);
    }

    bool AccessController::GrantUser(const std::string &database_name,
                                     const std::string &user_id,
                                     Permission permission,
                                     std::string &error_message) {
        if (!accounts_.contains(user_id)) {
            error_message = "user not found";
            return false;
        }
        database_grants_[database_name].user_permissions[user_id] |=
            PermissionMask(permission);
        return Persist(error_message);
    }

    bool AccessController::RevokeUser(const std::string &database_name,
                                      const std::string &user_id,
                                      Permission permission,
                                      std::string &error_message) {
        if (!database_grants_.contains(database_name)) {
            error_message = "database grant record not found";
            return false;
        }
        database_grants_[database_name].user_permissions[user_id] &=
            ~PermissionMask(permission);
        return Persist(error_message);
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
        const auto second = token.find('.', first == std::string::npos ? 0 : first + 1);
        if (first == std::string::npos || second == std::string::npos ||
            first == second || token.find('.', second + 1) != std::string::npos) {
            return std::nullopt;
        }
        const std::string header_b64 = token.substr(0, first);
        const std::string payload_b64 = token.substr(first + 1, second - first - 1);
        const std::string signature_b64 = token.substr(second + 1);
        if (header_b64.empty() || payload_b64.empty() || signature_b64.empty()) {
            return std::nullopt;
        }

        const auto header_json = Base64UrlDecode(header_b64);
        const auto payload_json = Base64UrlDecode(payload_b64);
        if (!header_json.has_value() || !payload_json.has_value()) {
            return std::nullopt;
        }
        if (*header_json != kJwtHeaderJson) {
            return std::nullopt;
        }

        const std::string signing_input = header_b64 + "." + payload_b64;
        if (SignTokenPayload(signing_input) != signature_b64) {
            return std::nullopt;
        }

        const auto user_id = ExtractStringClaim(*payload_json, "sub");
        const auto expires_unix = ExtractNumericClaim(*payload_json, "exp");
        if (!user_id.has_value() || !expires_unix.has_value()) {
            return std::nullopt;
        }

        const auto now_unix = std::chrono::duration_cast<std::chrono::seconds>(
                                  std::chrono::system_clock::now().time_since_epoch())
                                  .count();
        if (now_unix > *expires_unix) {
            return std::nullopt;
        }
        if (!accounts_.contains(*user_id)) {
            return std::nullopt;
        }
        return user_id;
    }

    bool AccessController::HasAccounts() const { return !accounts_.empty(); }

    bool AccessController::IsAdmin(const std::string &user_id) const {
        auto it = accounts_.find(user_id);
        if (it == accounts_.end()) {
            return false;
        }
        return std::find(it->second.groups.begin(), it->second.groups.end(),
                         "admin") != it->second.groups.end();
    }

    bool AccessController::Authorize(const std::string &user_id,
                                     const std::string &database_name,
                                     Permission permission) const {
        auto it = accounts_.find(user_id);
        if (it == accounts_.end()) {
            return false;
        }
        if (IsAdmin(user_id)) {
            return true;
        }
        auto grants_it = database_grants_.find(database_name);
        if (grants_it == database_grants_.end()) {
            return false;
        }
        if (HasPermissionInMask(grants_it->second.default_permissions, permission)) {
            return true;
        }
        for (const auto &group : it->second.groups) {
            auto group_it = grants_it->second.group_permissions.find(group);
            if (group_it != grants_it->second.group_permissions.end() &&
                HasPermissionInMask(group_it->second, permission)) {
                return true;
            }
        }
        auto user_it = grants_it->second.user_permissions.find(user_id);
        if (user_it != grants_it->second.user_permissions.end() &&
            HasPermissionInMask(user_it->second, permission)) {
            return true;
        }
        return false;
    }

    std::vector<std::string>
    AccessController::UserGroups(const std::string &user_id) const {
        auto it = accounts_.find(user_id);
        if (it == accounts_.end()) {
            return {};
        }
        return it->second.groups;
    }

    bool AccessController::Persist(std::string &error_message) const {
        std::filesystem::create_directories(root_path_);
        {
            std::ofstream accounts_out(AccountsFilePath(root_path_), std::ios::trunc);
            if (!accounts_out.is_open()) {
                error_message = "failed to persist accounts";
                return false;
            }
            accounts_out << "FORMAT\t1\n";
            for (const auto &[user_id, record] : accounts_) {
                accounts_out << "ACCOUNT\tuser_id=" << EscapeField(user_id)
                             << "\tsalt=" << EscapeField(record.salt)
                             << "\tpassword_hash=" << EscapeField(record.password_hash)
                             << "\tgroups=" << JoinGroups(record.groups) << "\n";
            }
            for (const auto &group : groups_) {
                accounts_out << "GROUP\tname=" << EscapeField(group) << "\n";
            }
        }
        {
            std::ofstream grants_out(GrantsFilePath(root_path_), std::ios::trunc);
            if (!grants_out.is_open()) {
                error_message = "failed to persist grants";
                return false;
            }
            grants_out << "FORMAT\t1\n";
            for (const auto &[database_name, record] : database_grants_) {
                grants_out << "DEFAULT\tdb=" << EscapeField(database_name)
                           << "\tmask=" << record.default_permissions << "\n";
                for (const auto &[group_name, mask] : record.group_permissions) {
                    grants_out << "GROUP_GRANT\tdb=" << EscapeField(database_name)
                               << "\tgroup=" << EscapeField(group_name)
                               << "\tmask=" << mask << "\n";
                }
                for (const auto &[user_id, mask] : record.user_permissions) {
                    grants_out << "USER_GRANT\tdb=" << EscapeField(database_name)
                               << "\tuser=" << EscapeField(user_id)
                               << "\tmask=" << mask << "\n";
                }
            }
        }
        error_message.clear();
        return true;
    }

    bool AccessController::Load() {
        accounts_.clear();
        groups_.clear();
        database_grants_.clear();

        std::ifstream accounts_in(AccountsFilePath(root_path_));
        if (accounts_in.is_open()) {
            std::string line;
            while (std::getline(accounts_in, line)) {
                if (line.empty()) {
                    continue;
                }
                const auto parts = SplitByTab(line);
                if (parts.empty() || parts[0] == "FORMAT") {
                    continue;
                }
                if (parts[0] == "ACCOUNT") {
                    AccountRecord record;
                    for (std::size_t index = 1; index < parts.size(); ++index) {
                        const auto split = parts[index].find('=');
                        if (split == std::string::npos) {
                            continue;
                        }
                        const std::string key = parts[index].substr(0, split);
                        const std::string value =
                            UnescapeField(parts[index].substr(split + 1));
                        if (key == "user_id") {
                            record.user_id = value;
                        } else if (key == "salt") {
                            record.salt = value;
                        } else if (key == "password_hash") {
                            record.password_hash = value;
                        } else if (key == "groups") {
                            record.groups = SplitGroups(parts[index].substr(split + 1));
                        }
                    }
                    if (!record.user_id.empty()) {
                        accounts_.emplace(record.user_id, record);
                        for (const auto &group : record.groups) {
                            groups_.insert(group);
                        }
                    }
                } else if (parts[0] == "GROUP" && parts.size() >= 2) {
                    const auto split = parts[1].find('=');
                    if (split != std::string::npos) {
                        groups_.insert(
                            UnescapeField(parts[1].substr(split + 1)));
                    }
                }
            }
        }

        std::ifstream grants_in(GrantsFilePath(root_path_));
        if (grants_in.is_open()) {
            std::string line;
            while (std::getline(grants_in, line)) {
                if (line.empty()) {
                    continue;
                }
                const auto parts = SplitByTab(line);
                if (parts.empty() || parts[0] == "FORMAT") {
                    continue;
                }
                std::string database_name;
                std::string group_name;
                std::string user_id;
                std::uint32_t mask = 0;
                for (std::size_t index = 1; index < parts.size(); ++index) {
                    const auto split = parts[index].find('=');
                    if (split == std::string::npos) {
                        continue;
                    }
                    const std::string key = parts[index].substr(0, split);
                    const std::string value =
                        UnescapeField(parts[index].substr(split + 1));
                    if (key == "db") {
                        database_name = value;
                    } else if (key == "group") {
                        group_name = value;
                    } else if (key == "user") {
                        user_id = value;
                    } else if (key == "mask") {
                        mask = static_cast<std::uint32_t>(std::stoul(value));
                    }
                }
                if (database_name.empty()) {
                    continue;
                }
                auto &record = database_grants_[database_name];
                if (parts[0] == "DEFAULT") {
                    record.default_permissions = mask;
                } else if (parts[0] == "GROUP_GRANT") {
                    record.group_permissions[group_name] = mask;
                } else if (parts[0] == "USER_GRANT") {
                    record.user_permissions[user_id] = mask;
                }
            }
        }
        return true;
    }

    bool AccessController::EnsureJwtSecret() {
        std::ifstream input(SecretFilePath(root_path_));
        if (input.is_open()) {
            std::getline(input, jwt_secret_);
            if (!jwt_secret_.empty()) {
                return true;
            }
        }
        jwt_secret_ = NewSecret();
        std::ofstream output(SecretFilePath(root_path_), std::ios::trunc);
        if (!output.is_open()) {
            return false;
        }
        output << jwt_secret_;
        return true;
    }

    std::string AccessController::AccountsFilePath(const std::string &root_path) {
        return root_path + "/accounts.tsv";
    }

    std::string AccessController::GrantsFilePath(const std::string &root_path) {
        return root_path + "/rbac_grants.tsv";
    }

    std::string AccessController::SecretFilePath(const std::string &root_path) {
        return root_path + "/jwt_secret.txt";
    }

    std::string AccessController::HashPassword(const std::string &salt,
                                               const std::string &password) const {
        unsigned char digest[EVP_MAX_MD_SIZE]{};
        unsigned int digest_size = 0;
        const std::string input = salt + ":" + password;
        const auto *raw =
            HMAC(EVP_sha256(), salt.data(), static_cast<int>(salt.size()),
                 reinterpret_cast<const unsigned char *>(input.data()),
                 input.size(), digest, &digest_size);
        if (raw == nullptr) {
            return {};
        }
        std::ostringstream output;
        output << std::hex << std::setfill('0');
        for (unsigned int index = 0; index < digest_size; ++index) {
            output << std::setw(2) << static_cast<int>(digest[index]);
        }
        return output.str();
    }

    std::string AccessController::NewSalt() const {
        unsigned char bytes[16]{};
        if (RAND_bytes(bytes, sizeof(bytes)) != 1) {
            static thread_local std::mt19937_64 rng{std::random_device{}()};
            std::uniform_int_distribution<std::uint64_t> distribution;
            std::ostringstream fallback;
            fallback << std::hex << distribution(rng);
            return fallback.str();
        }
        std::ostringstream output;
        output << std::hex << std::setfill('0');
        for (unsigned char byte : bytes) {
            output << std::setw(2) << static_cast<int>(byte);
        }
        return output.str();
    }

    std::string AccessController::NewSecret() const {
        unsigned char bytes[32]{};
        if (RAND_bytes(bytes, sizeof(bytes)) != 1) {
            return NewSalt() + NewSalt();
        }
        std::ostringstream output;
        output << std::hex << std::setfill('0');
        for (unsigned char byte : bytes) {
            output << std::setw(2) << static_cast<int>(byte);
        }
        return output.str();
    }

    std::string AccessController::IssueToken(const std::string &user_id,
                                             std::int64_t expires_unix) const {
        const std::string header_b64 = Base64UrlEncode(kJwtHeaderJson);
        const std::string payload_json = std::string("{\"sub\":\"") + user_id +
                                         "\",\"exp\":" +
                                         std::to_string(expires_unix) + "}";
        const std::string payload_b64 = Base64UrlEncode(payload_json);
        const std::string signing_input = header_b64 + "." + payload_b64;
        return signing_input + "." + SignTokenPayload(signing_input);
    }

    std::string
    AccessController::SignTokenPayload(const std::string &payload) const {
        unsigned int digest_size = 0;
        unsigned char digest[EVP_MAX_MD_SIZE]{};
        const auto *raw =
            HMAC(EVP_sha256(), jwt_secret_.data(),
                 static_cast<int>(jwt_secret_.size()),
                 reinterpret_cast<const unsigned char *>(payload.data()),
                 payload.size(), digest, &digest_size);
        if (raw == nullptr) {
            return {};
        }
        return Base64UrlEncode(
            std::string(reinterpret_cast<const char *>(digest), digest_size));
    }

    std::string AccessController::Base64UrlEncode(const std::string &data) const {
        std::string output;
        output.reserve(((data.size() + 2) / 3) * 4);
        std::size_t index = 0;
        while (index + 3 <= data.size()) {
            const auto value = (static_cast<std::uint32_t>(
                                    static_cast<unsigned char>(data[index]))
                                << 16U) |
                               (static_cast<std::uint32_t>(
                                    static_cast<unsigned char>(data[index + 1]))
                                << 8U) |
                               static_cast<std::uint32_t>(
                                   static_cast<unsigned char>(data[index + 2]));
            output.push_back(kBase64UrlAlphabet[(value >> 18U) & 0x3FU]);
            output.push_back(kBase64UrlAlphabet[(value >> 12U) & 0x3FU]);
            output.push_back(kBase64UrlAlphabet[(value >> 6U) & 0x3FU]);
            output.push_back(kBase64UrlAlphabet[value & 0x3FU]);
            index += 3;
        }
        const std::size_t rem = data.size() - index;
        if (rem == 1) {
            const auto value = static_cast<std::uint32_t>(
                                   static_cast<unsigned char>(data[index]))
                               << 16U;
            output.push_back(kBase64UrlAlphabet[(value >> 18U) & 0x3FU]);
            output.push_back(kBase64UrlAlphabet[(value >> 12U) & 0x3FU]);
        } else if (rem == 2) {
            const auto value =
                (static_cast<std::uint32_t>(
                     static_cast<unsigned char>(data[index]))
                 << 16U) |
                (static_cast<std::uint32_t>(
                     static_cast<unsigned char>(data[index + 1]))
                 << 8U);
            output.push_back(kBase64UrlAlphabet[(value >> 18U) & 0x3FU]);
            output.push_back(kBase64UrlAlphabet[(value >> 12U) & 0x3FU]);
            output.push_back(kBase64UrlAlphabet[(value >> 6U) & 0x3FU]);
        }
        return output;
    }

    std::optional<std::string>
    AccessController::Base64UrlDecode(const std::string &data) const {
        std::array<int, 256> reverse{};
        reverse.fill(-1);
        for (int i = 0; i < 64; ++i) {
            reverse[static_cast<unsigned char>(kBase64UrlAlphabet[i])] = i;
        }
        std::vector<unsigned char> bytes;
        bytes.reserve((data.size() * 3) / 4 + 3);
        std::uint32_t value = 0;
        int bits = 0;
        for (char ch : data) {
            const int decoded = reverse[static_cast<unsigned char>(ch)];
            if (decoded < 0) {
                return std::nullopt;
            }
            value = (value << 6U) | static_cast<std::uint32_t>(decoded);
            bits += 6;
            while (bits >= 8) {
                bits -= 8;
                bytes.push_back(static_cast<unsigned char>(
                    (value >> static_cast<unsigned int>(bits)) & 0xFFU));
            }
        }
        if (bits == 1 || bits == 3 || bits == 5) {
            return std::nullopt;
        }
        return std::string(reinterpret_cast<const char *>(bytes.data()),
                           bytes.size());
    }

    std::optional<std::int64_t>
    AccessController::ExtractNumericClaim(const std::string &json,
                                          const std::string &key) const {
        const std::string marker = "\"" + key + "\":";
        const auto position = json.find(marker);
        if (position == std::string::npos) {
            return std::nullopt;
        }
        std::size_t index = position + marker.size();
        while (index < json.size() &&
               std::isspace(static_cast<unsigned char>(json[index]))) {
            ++index;
        }
        std::size_t end = index;
        while (end < json.size() &&
               (std::isdigit(static_cast<unsigned char>(json[end])) ||
                json[end] == '-')) {
            ++end;
        }
        if (end == index) {
            return std::nullopt;
        }
        try {
            return std::stoll(json.substr(index, end - index));
        } catch (...) {
            return std::nullopt;
        }
    }

    std::optional<std::string>
    AccessController::ExtractStringClaim(const std::string &json,
                                         const std::string &key) const {
        const std::string marker = "\"" + key + "\":\"";
        const auto position = json.find(marker);
        if (position == std::string::npos) {
            return std::nullopt;
        }
        const std::size_t begin = position + marker.size();
        const auto end = json.find('"', begin);
        if (end == std::string::npos) {
            return std::nullopt;
        }
        return json.substr(begin, end - begin);
    }

    bool AccessController::HasPermissionInMask(std::uint32_t mask,
                                               Permission permission) const {
        return (mask & PermissionMask(permission)) != 0U;
    }

    std::uint32_t AccessController::PermissionMask(Permission permission) {
        return static_cast<std::uint32_t>(permission);
    }

    std::optional<Permission>
    AccessController::ParsePermissionName(const std::string &permission_name) {
        const auto upper = ToUpper(permission_name);
        if (upper == "READ") {
            return Permission::kRead;
        }
        if (upper == "WRITE") {
            return Permission::kWrite;
        }
        if (upper == "CREATE_TABLE") {
            return Permission::kCreateTable;
        }
        if (upper == "DROP_TABLE") {
            return Permission::kDropTable;
        }
        if (upper == "DROP_DATABASE") {
            return Permission::kDropDatabase;
        }
        return std::nullopt;
    }

} // namespace dbms::catalog
