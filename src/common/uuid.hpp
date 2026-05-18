#pragma once

// this file declares guid generation helpers for async jobs and tracing.
#include <string>

namespace dbms::common {

  class UuidGenerator {
  public:
    [[nodiscard]] static std::string NewGuidV4();
  };

} // namespace dbms::common
