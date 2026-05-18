#include "server/entrypoint.hpp"

// this file routes requests through the dbms engine and preserves session
// state.
namespace dbms::server {

    EntrypointServer::EntrypointServer(std::string root_path)
        : engine_(std::move(root_path)) {}

    core::SessionContext &
    EntrypointServer::GetOrCreateSession(const std::string &client_id) {
        auto [it, inserted] = sessions_.try_emplace(client_id);
        if (inserted) {
            it->second.client_id = client_id;
        }
        return it->second;
    }

    network::ResponseEnvelope
    EntrypointServer::HandleRequest(const network::RequestEnvelope &request) {
        auto &session = GetOrCreateSession(request.client_id);
        session.client_id = request.client_id;

        auto result = engine_.ExecuteSql(session, request.payload);
        if (!result.ok()) {
            return {.status_code = 400, .payload = "Request failed"};
        }

        return {.status_code = 200, .payload = result.value->message};
    }

} // namespace dbms::server
