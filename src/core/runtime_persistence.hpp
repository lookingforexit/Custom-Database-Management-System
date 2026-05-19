#pragma once

#include <string>

#include "core/runtime_state.hpp"

namespace dbms::core {

    class RuntimePersistence {
      public:
        explicit RuntimePersistence(std::string root_path);

        bool Load(RuntimeState &runtime_state) const;
        bool Save(const RuntimeState &runtime_state) const;

      private:
        std::string StateFilePath() const;
        std::string root_path_;
    };

} // namespace dbms::core
