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

} // namespace dbms::network
