#include "common/error_contract.hpp"

namespace dbms::common {

    const char *ErrorCodeName(ErrorCode code) {
        switch (code) {
            case ErrorCode::kOk:
                return "OK";
            case ErrorCode::kParseError:
                return "PARSE_ERROR";
            case ErrorCode::kSemanticError:
                return "SEMANTIC_ERROR";
            case ErrorCode::kValidationError:
                return "VALIDATION_ERROR";
            case ErrorCode::kStorageError:
                return "STORAGE_ERROR";
            case ErrorCode::kAuthorizationError:
                return "AUTHORIZATION_ERROR";
            case ErrorCode::kNetworkError:
                return "NETWORK_ERROR";
            case ErrorCode::kNotFound:
                return "NOT_FOUND";
            case ErrorCode::kAlreadyExists:
                return "ALREADY_EXISTS";
            case ErrorCode::kConstraintViolation:
                return "CONSTRAINT_VIOLATION";
            case ErrorCode::kTypeMismatch:
                return "TYPE_MISMATCH";
            case ErrorCode::kNotImplemented:
                return "NOT_IMPLEMENTED";
        }
        return "UNKNOWN";
    }

    std::string EscapeJsonText(const std::string &value) {
        std::string escaped;
        escaped.reserve(value.size());
        for (char ch : value) {
            if (ch == '"' || ch == '\\') {
                escaped.push_back('\\');
            }
            escaped.push_back(ch);
        }
        return escaped;
    }

    std::string
    FormatErrorContract(const Error &error, const std::string &sql) {
        return "type=" + std::string(ErrorCodeName(error.code)) + " code=" +
               std::to_string(static_cast<int>(error.code)) + " message=" +
               error.message + " sql=\"" + EscapeJsonText(sql) + "\"";
    }

} // namespace dbms::common
