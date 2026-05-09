#pragma once

// this file will provide shared time aliases and timestamp formatting.
#include <chrono>
#include <string>

namespace dbms::common {

using Clock = std::chrono::system_clock;
using TimePoint = Clock::time_point;

[[nodiscard]] std::string FormatTimestamp(TimePoint time_point);

}  // namespace dbms::common
