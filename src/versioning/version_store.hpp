#pragma once

// this file defines append-only history storage used by revert support.
#include <string>
#include <vector>

namespace dbms::versioning {

  struct ChangeRecord {
    std::string database_name;
    std::string table_name;
    std::string operation;
    std::string timestamp;
  };

  class VersionStore {
  public:
    void Append(ChangeRecord record);
    [[nodiscard]] std::vector<ChangeRecord>
    HistoryForTable(const std::string &database_name,
                    const std::string &table_name) const;

  private:
    std::vector<ChangeRecord> records_;
  };

} // namespace dbms::versioning
