#pragma once

#include <string>

#include "core/runtime_state.hpp"
#include "versioning/version_store.hpp"

namespace dbms::core {

    class RuntimePersistence {
      public:
        explicit RuntimePersistence(std::string root_path);

        bool Load(RuntimeState &runtime_state,
                  versioning::VersionStore &version_store) const;
        bool Save(const RuntimeState &runtime_state,
                  const versioning::VersionStore &version_store) const;

      private:
        std::string StateFilePath() const;
        std::string HistoryFilePath() const;
        std::string root_path_;
    };

} // namespace dbms::core
