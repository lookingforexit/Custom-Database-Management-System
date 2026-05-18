#pragma once

// this file defines the entrypoint facade that manages requests and client
// sessions.
#include <string>
#include <unordered_map>

#include "core/dbms_engine.hpp"
#include "network/protocol.hpp"

namespace dbms::server {

  class EntrypointServer {
  public:
    explicit EntrypointServer(std::string root_path);

    [[nodiscard]] network::ResponseEnvelope
    HandleRequest(const network::RequestEnvelope &request);

  private:
    core::SessionContext &GetOrCreateSession(const std::string &client_id);

    core::DbmsEngine engine_;
    std::unordered_map<std::string, core::SessionContext> sessions_;
  };

} // namespace dbms::server
