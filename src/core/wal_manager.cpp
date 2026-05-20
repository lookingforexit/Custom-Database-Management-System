#include "core/wal_manager.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <chrono>

namespace dbms::core {

    namespace {
        constexpr const char *kHeaderPrefix = "WAL\t";
        constexpr const char *kCurrentVersion = "2";

        bool IsSupportedWalVersion(const std::string &version) {
            return version == "1" || version == "2";
        }
    }

    WalManager::WalManager(std::string root_path) : root_path_(std::move(root_path)) {}

    bool WalManager::AppendSql(const std::string &sql) const {
        std::filesystem::create_directories(root_path_);
        std::ofstream out(WalFilePath(), std::ios::app);
        if (!out.is_open()) {
            return false;
        }
        if (out.tellp() == 0) {
            out << kHeaderPrefix << kCurrentVersion << "\n";
        }
        out << "SQL\t" << Escape(sql) << "\n";
        return out.good();
    }

    bool WalManager::AppendTransaction(
        const std::vector<std::string> &statements) const {
        std::filesystem::create_directories(root_path_);
        std::ofstream out(WalFilePath(), std::ios::app);
        if (!out.is_open()) {
            return false;
        }
        if (out.tellp() == 0) {
            out << kHeaderPrefix << kCurrentVersion << "\n";
        }
        out << "TX_BEGIN\n";
        for (const auto &statement : statements) {
            out << "TX_SQL\t" << Escape(statement) << "\n";
        }
        out << "TX_END\n";
        return out.good();
    }

    bool WalManager::LoadAll(std::vector<WalEntry> &entries) const {
        entries.clear();
        std::ifstream in(WalFilePath());
        if (!in.is_open()) {
            return true;
        }
        std::string line;
        if (!std::getline(in, line)) {
            return true;
        }
        if (line.rfind(kHeaderPrefix, 0) != 0) {
            return false;
        }
        const std::string version = line.substr(std::string(kHeaderPrefix).size());
        if (!IsSupportedWalVersion(version)) {
            return false;
        }

        WalEntry current_tx;
        bool in_tx = false;
        while (std::getline(in, line)) {
            if (line.empty()) {
                continue;
            }
            if (line == "TX_BEGIN") {
                if (in_tx) {
                    return false;
                }
                in_tx = true;
                current_tx = WalEntry{.type = WalEntry::Type::kTransaction, .statements = {}};
                continue;
            }
            if (line == "TX_END") {
                if (!in_tx) {
                    return false;
                }
                entries.push_back(current_tx);
                in_tx = false;
                continue;
            }
            if (line.rfind("SQL\t", 0) == 0) {
                if (in_tx) {
                    return false;
                }
                entries.push_back(WalEntry{
                    .type = WalEntry::Type::kSql,
                    .statements = {Unescape(line.substr(4))},
                });
                continue;
            }
            if (line.rfind("TX_SQL\t", 0) == 0) {
                if (!in_tx) {
                    return false;
                }
                current_tx.statements.push_back(Unescape(line.substr(7)));
                continue;
            }
            return false;
        }
        // Incomplete TX at EOF is treated as uncommitted and discarded (undo).
        return true;
    }

    bool WalManager::Reset() const {
        std::filesystem::create_directories(root_path_);
        std::ofstream out(WalFilePath(), std::ios::trunc);
        if (!out.is_open()) {
            return false;
        }
        out << kHeaderPrefix << kCurrentVersion << "\n";
        return out.good();
    }

    bool WalManager::QuarantineCorrupted() const {
        const auto wal_path = WalFilePath();
        if (!std::filesystem::exists(wal_path)) {
            return Reset();
        }
        const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();
        const auto corrupt_path =
            wal_path + ".corrupt." + std::to_string(now);
        std::error_code rename_error;
        std::filesystem::rename(wal_path, corrupt_path, rename_error);
        if (rename_error) {
            return false;
        }
        return Reset();
    }

    std::string WalManager::WalFilePath() const { return root_path_ + "/wal.log"; }

    std::string WalManager::Escape(const std::string &value) {
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

    std::string WalManager::Unescape(const std::string &value) {
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

} // namespace dbms::core
