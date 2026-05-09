#pragma once

// this file declares the entrypoint server request handling facade.
#include "execution/execution_engine.hpp"
#include "network/protocol.hpp"
#include "parser/parser.hpp"
#include "planner/planner.hpp"

namespace dbms::server {

class EntrypointServer {
public:
    [[nodiscard]] network::ResponseEnvelope HandleRequest(
        const network::RequestEnvelope& request) const;

private:
    parser::Parser parser_;
    planner::Planner planner_;
    execution::ExecutionEngine execution_;
};

}  // namespace dbms::server
