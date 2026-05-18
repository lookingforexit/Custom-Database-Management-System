#pragma once

// this file declares shared time aliases and timestamp formatting helpers.
#include <chrono>
#include <string>

namespace dbms::common {

  using Clock = std::chrono::system_clock;
  using TimePoint = Clock::time_point;

  [[nodiscard]] std::string FormatTimestamp(TimePoint time_point);

} // namespace dbms::common
