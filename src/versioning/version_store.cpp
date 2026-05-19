#include "versioning/version_store.hpp"

#include <chrono>
#include <iomanip>
#include <sstream>

// this file implements change-log storage used for time-based restoration.
namespace dbms::versioning {

    std::string VersionStore::Append(ChangeRecord record) {
        if (record.timestamp.empty()) {
            record.timestamp = GenerateTimestamp();
        }
        records_.push_back(std::move(record));
        return records_.back().timestamp;
    }

    void VersionStore::ReplaceAll(std::vector<ChangeRecord> records) {
        records_ = std::move(records);
    }

    void VersionStore::Clear() { records_.clear(); }

    const std::vector<ChangeRecord> &VersionStore::AllRecords() const {
        return records_;
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

    std::optional<ChangeRecord> VersionStore::SnapshotAtOrBefore(
        const std::string &database_name, const std::string &table_name,
        const std::string &timestamp) const {
        std::optional<ChangeRecord> best;
        for (const auto &record : records_) {
            if (record.database_name != database_name ||
                record.table_name != table_name) {
                continue;
            }
            if (record.timestamp > timestamp) {
                continue;
            }
            if (!best.has_value() || best->timestamp < record.timestamp) {
                best = record;
            }
        }
        return best;
    }

    std::optional<ChangeRecord> VersionStore::SnapshotExact(
        const std::string &database_name, const std::string &table_name,
        const std::string &timestamp) const {
        for (const auto &record : records_) {
            if (record.database_name == database_name &&
                record.table_name == table_name &&
                record.timestamp == timestamp) {
                return record;
            }
        }
        return std::nullopt;
    }

    std::optional<ChangeRecord>
    VersionStore::LatestSnapshot(const std::string &database_name,
                                 const std::string &table_name) const {
        std::optional<ChangeRecord> latest;
        for (const auto &record : records_) {
            if (record.database_name != database_name ||
                record.table_name != table_name) {
                continue;
            }
            if (!latest.has_value() || latest->timestamp < record.timestamp) {
                latest = record;
            }
        }
        return latest;
    }

    std::string VersionStore::GenerateTimestamp() {
        const auto now = std::chrono::system_clock::now();
        const auto seconds =
            std::chrono::time_point_cast<std::chrono::seconds>(now);
        const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(
                                now - seconds)
                                .count();
        const auto time_value = std::chrono::system_clock::to_time_t(now);
        std::tm tm{};
#ifdef _WIN32
        localtime_s(&tm, &time_value);
#else
        localtime_r(&time_value, &tm);
#endif

        std::ostringstream output;
        output << std::put_time(&tm, "%Y.%m.%d-%H:%M:%S") << "."
               << std::setw(6) << std::setfill('0') << micros;
        return output.str();
    }

} // namespace dbms::versioning
