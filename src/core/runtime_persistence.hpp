#pragma once

#include <string>

#include "core/runtime_state.hpp"
#include "storage/string_pool.hpp"
#include "versioning/version_store.hpp"

namespace dbms::core {

    class RuntimePersistence {
      public:
        explicit RuntimePersistence(std::string root_path);

        bool Load(RuntimeState &runtime_state,
                  versioning::VersionStore &version_store,
                  storage::StringPool &string_pool) const;
        bool Save(const RuntimeState &runtime_state,
                  const versioning::VersionStore &version_store,
                  const storage::StringPool &string_pool) const;

      private:
        std::string StateFilePath() const;
        std::string HistoryFilePath() const;
        std::string root_path_;
    };

} // namespace dbms::core
