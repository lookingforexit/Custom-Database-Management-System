#pragma once

// this file defines a simple result wrapper for fallible operations.
#include <optional>

#include "common/error.hpp"

namespace dbms::common {

    template <typename T> struct Result {
        std::optional<T> value;
        std::optional<Error> error;

        [[nodiscard]] bool ok() const {
            return value.has_value() && !error.has_value();
        }
    };

} // namespace dbms::common
