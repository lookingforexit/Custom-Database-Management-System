#include "versioning/version_store.hpp"

// this file implements change-log storage used for time-based restoration.
namespace dbms::versioning {

    void VersionStore::Append(ChangeRecord record) {
        records_.push_back(std::move(record));
    }

    std::vector<ChangeRecord>
    VersionStore::HistoryForTable(const std::string &database_name,
                                  const std::string &table_name) const {
        std::vector<ChangeRecord> result;
        for (const auto &record : records_) {
            if (record.database_name == database_name &&
                record.table_name == table_name) {
                result.push_back(record);
            }
        }
        return result;
    }

} // namespace dbms::versioning
