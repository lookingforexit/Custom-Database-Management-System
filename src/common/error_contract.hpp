#pragma once

#include <string>

#include "common/error.hpp"

namespace dbms::common {

    [[nodiscard]] const char *ErrorCodeName(ErrorCode code);
    [[nodiscard]] std::string EscapeJsonText(const std::string &value);
    [[nodiscard]] std::string
    FormatErrorContract(const Error &error, const std::string &sql);

} // namespace dbms::common
