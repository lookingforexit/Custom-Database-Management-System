#pragma once

// this file defines request and response envelopes for network communication.
#include <string>

namespace dbms::network {

    struct RequestEnvelope {
        std::string client_id;
        std::string jwt_token;
        std::string payload;
    };

    struct ResponseEnvelope {
        int status_code{200};
        std::string payload;
    };

    [[nodiscard]] std::string SerializeRequest(const RequestEnvelope &request);
    [[nodiscard]] std::string SerializeResponse(const ResponseEnvelope &response);
    [[nodiscard]] bool DeserializeRequest(const std::string &wire,
                                          RequestEnvelope &request);
    [[nodiscard]] bool DeserializeResponse(const std::string &wire,
                                           ResponseEnvelope &response);

} // namespace dbms::network
