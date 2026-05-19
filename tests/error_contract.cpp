#include <string>

#include "common/error_contract.hpp"
#include "common/error.hpp"
#include "server/entrypoint.hpp"

int main() {
    {
        const dbms::common::Error error{
            .code = dbms::common::ErrorCode::kParseError,
            .message = "bad token",
        };
        const std::string formatted =
            dbms::common::FormatErrorContract(error, "SeLeCt 1;");
        if (formatted.find("type=PARSE_ERROR") == std::string::npos) return 1;
        if (formatted.find("code=1") == std::string::npos) return 1;
        if (formatted.find("message=bad token") == std::string::npos) return 1;
        if (formatted.find("sql=\"SeLeCt 1;\"") == std::string::npos) return 1;
    }

    {
        dbms::server::EntrypointServer server("./test_data_error_contract");
        const dbms::network::RequestEnvelope request{
            .client_id = "err",
            .jwt_token = "",
            .payload = "SeLeCt * FROM t;",
        };
        const auto response = server.HandleRequest(request);
        if (response.status_code != 400) return 1;
        if (response.payload.find("type=PARSE_ERROR") == std::string::npos) return 1;
        if (response.payload.find("code=1") == std::string::npos) return 1;
        if (response.payload.find("message=") == std::string::npos) return 1;
        if (response.payload.find("sql=\"SeLeCt * FROM t;\"") ==
            std::string::npos) {
            return 1;
        }
    }

    return 0;
}
