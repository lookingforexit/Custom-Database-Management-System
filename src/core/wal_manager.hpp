#pragma once

#include <string>
#include <vector>

namespace dbms::core {

    struct WalEntry {
        enum class Type { kSql, kTransaction };
        Type type{Type::kSql};
        std::vector<std::string> statements;
    };

    class WalManager {
      public:
        explicit WalManager(std::string root_path);

        bool AppendSql(const std::string &sql) const;
        bool AppendTransaction(const std::vector<std::string> &statements) const;
        bool LoadAll(std::vector<WalEntry> &entries) const;
        bool Reset() const;
        bool QuarantineCorrupted() const;

      private:
        std::string WalFilePath() const;
        static std::string Escape(const std::string &value);
        static std::string Unescape(const std::string &value);
        std::string root_path_;
    };

} // namespace dbms::core
