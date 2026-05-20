#include "catalog/rbac.hpp"

// this file implements permission checks for rbac authorization decisions.
#include <chrono>
#include <array>
#include <cctype>
#include <cstring>
#include <iomanip>
#include <random>
#include <sstream>
#include <vector>

#include <openssl/evp.h>
#include <openssl/hmac.h>

namespace dbms::catalog {

    namespace {
        constexpr const char *kJwtSecret = "cw2026_demo_secret";
        constexpr const char *kJwtHeaderJson = R"({"alg":"HS256","typ":"JWT"})";
        constexpr char kBase64UrlAlphabet[] =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    } // namespace

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
            HMAC(EVP_sha256(), kJwtSecret, std::strlen(kJwtSecret),
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
