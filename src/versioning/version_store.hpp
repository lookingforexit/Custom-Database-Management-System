#pragma once

// this file defines append-only delta history used by temporal REVERT.
#include <optional>
#include <string>
#include <vector>

#include "catalog/schema.hpp"
#include "common/types.hpp"

namespace dbms::versioning {

    enum class ChangeKind {
        kCreateDatabase,
        kDropDatabase,
        kCreateTable,
        kDropTable,
        kInsertRow,
        kUpdateRow,
        kDeleteRow,
    };

    struct ChangeRecord {
        ChangeKind kind{ChangeKind::kInsertRow};
        std::string database_name;
        std::string table_name;
        std::string timestamp;
        common::RowId row_id{common::kInvalidRowId};
        std::optional<catalog::TableSchema> schema;
        std::optional<common::RowData> before_row;
        std::optional<common::RowData> after_row;
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
        [[nodiscard]] std::optional<std::string>
        LatestTimestampForTable(const std::string &database_name,
                                const std::string &table_name) const;
        [[nodiscard]] std::optional<std::string>
        LatestTimestampAtOrBefore(const std::string &database_name,
                                  const std::string &table_name,
                                  const std::string &timestamp) const;
        [[nodiscard]] bool HasExactTimestampForTable(
            const std::string &database_name, const std::string &table_name,
            const std::string &timestamp) const;

      private:
        [[nodiscard]] static std::string GenerateTimestamp();
        std::vector<ChangeRecord> records_;
    };

} // namespace dbms::versioning
