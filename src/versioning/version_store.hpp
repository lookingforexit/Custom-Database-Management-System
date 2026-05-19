#pragma once

// this file defines append-only history storage used by revert support.
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "common/types.hpp"

namespace dbms::versioning {

    struct ChangeRecord {
        std::string database_name;
        std::string table_name;
        std::string operation;
        std::string timestamp;
        std::vector<common::RowData> snapshot_rows;
    };

    class VersionStore {
      public:
        [[nodiscard]] std::string Append(ChangeRecord record);
        void ReplaceAll(std::vector<ChangeRecord> records);
        void Clear();
        [[nodiscard]] const std::vector<ChangeRecord> &AllRecords() const;
        [[nodiscard]] std::vector<ChangeRecord>
        HistoryForTable(const std::string &database_name,
                        const std::string &table_name) const;
        [[nodiscard]] std::optional<ChangeRecord>
        SnapshotAtOrBefore(const std::string &database_name,
                           const std::string &table_name,
                           const std::string &timestamp) const;
        [[nodiscard]] std::optional<ChangeRecord>
        SnapshotExact(const std::string &database_name,
                      const std::string &table_name,
                      const std::string &timestamp) const;
        [[nodiscard]] std::optional<ChangeRecord>
        LatestSnapshot(const std::string &database_name,
                       const std::string &table_name) const;

      private:
        [[nodiscard]] static std::string GenerateTimestamp();
        std::vector<ChangeRecord> records_;
    };

} // namespace dbms::versioning
