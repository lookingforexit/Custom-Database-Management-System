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

    template <typename T>
    Result<T> MakeSuccess(T value) {
        return {.value = std::move(value), .error = std::nullopt};
    }

    template <typename T>
    Result<T> MakeError(ErrorCode error_code, std::string message) {
        return {
            .value = std::nullopt,
            .error = Error{.code = error_code, .message = std::move(message)}
        };
    }

} // namespace dbms::common
