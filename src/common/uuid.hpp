#pragma once

// this file will generate guid v4 values for async jobs and tracing.
#include <string>

namespace dbms::common {

class UuidGenerator {
public:
    [[nodiscard]] static std::string NewGuidV4();
};

}  // namespace dbms::common
