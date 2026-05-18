#pragma once

#include <optional>
#include <string>

namespace dbms::core {

  struct SessionContext {
    std::string client_id;
    std::optional<std::string> user_id;
    std::string current_database;
  };

} // namespace dbms::core
