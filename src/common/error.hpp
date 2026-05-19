#pragma once

// this file defines shared error codes and user-facing error payloads.
#include <string>

namespace dbms::common {

    enum class ErrorCode {
        kOk,
        kParseError,
        kSemanticError,
        kValidationError,
        kStorageError,
        kAuthorizationError,
        kNetworkError,
        kNotFound,
        kAlreadyExists,
        kConstraintViolation,
        kTypeMismatch,
        kNotImplemented,
    };

    struct Error {
        ErrorCode code{ErrorCode::kOk};
        std::string message;
    };

} // namespace dbms::common
