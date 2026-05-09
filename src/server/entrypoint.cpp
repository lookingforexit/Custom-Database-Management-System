#include "server/entrypoint.hpp"

// this file will orchestrate parse, plan, auth, dispatch, and response flow.
namespace dbms::server {

network::ResponseEnvelope EntrypointServer::HandleRequest(
    const network::RequestEnvelope& request) const {
    auto parsed = parser_.Parse(request.payload);
    if (!parsed.ok()) {
        return {.status_code = 400, .payload = "Parse error"};
    }

    auto plan = planner_.BuildPlan(*parsed.value);
    if (!plan.ok()) {
        return {.status_code = 422, .payload = "Planning error"};
    }

    auto result = execution_.Execute(*plan.value);
    if (!result.ok()) {
        return {.status_code = 500, .payload = "Execution error"};
    }

    return {.status_code = 200, .payload = result.value->message};
}

}  // namespace dbms::server
