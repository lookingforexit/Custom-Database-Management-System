#include "server/entrypoint.hpp"

#include <chrono>
#include <filesystem>
#include <string>
#include <thread>

namespace {

    std::string ExtractValue(const std::string &payload, const std::string &key) {
        const std::string prefix = key + "=";
        const auto position = payload.find(prefix);
        if (position == std::string::npos) {
            return {};
        }
        auto start = position + prefix.size();
        auto end = payload.find(' ', start);
        if (end == std::string::npos) {
            end = payload.size();
        }
        return payload.substr(start, end - start);
    }

} // namespace

int main() {
    const std::string root = "./test_data_server_api_edges";
    std::filesystem::remove_all(root);
    dbms::server::EntrypointServer server(root);
    dbms::network::RequestEnvelope request{
        .client_id = "edge_client",
        .jwt_token = "",
        .payload = "",
    };

    // AUTH malformed commands.
    request.payload = "AUTH REGISTER";
    auto response = server.HandleRequest(request);
    if (response.status_code != 400) return 1;

    request.payload = "AUTH LOGIN";
    response = server.HandleRequest(request);
    if (response.status_code != 400) return 1;

    // Register/login and duplicate register.
    request.payload = "AUTH REGISTER admin pass admin";
    response = server.HandleRequest(request);
    if (response.status_code != 200) return 1;

    request.payload = "AUTH REGISTER admin pass admin";
    response = server.HandleRequest(request);
    if (response.status_code != 400) return 1;

    request.payload = "AUTH LOGIN admin wrong";
    response = server.HandleRequest(request);
    if (response.status_code != 401) return 1;

    request.payload = "AUTH LOGIN admin pass";
    response = server.HandleRequest(request);
    if (response.status_code != 200) return 1;
    const std::string token = ExtractValue(response.payload, "token");
    if (token.empty()) return 1;

    // Invalid token is blocked when accounts exist.
    request.jwt_token = "bad.token.value";
    request.payload = "CREATE DATABASE xdb;";
    response = server.HandleRequest(request);
    if (response.status_code != 401) return 1;

    // Valid token allows DDL for admin.
    request.jwt_token = token;
    request.payload = "CREATE DATABASE xdb;";
    response = server.HandleRequest(request);
    if (response.status_code != 200) return 1;

    // ASYNC invalid forms.
    request.payload = "ASYNC SUBMIT";
    response = server.HandleRequest(request);
    if (response.status_code != 400) return 1;

    request.payload = "ASYNC STATUS";
    response = server.HandleRequest(request);
    if (response.status_code != 400) return 1;

    request.payload = "ASYNC RESULT missing-id";
    response = server.HandleRequest(request);
    if (response.status_code != 404) return 1;

    // ASYNC failed SQL transitions to FAILED/400.
    request.payload = "ASYNC SUBMIT BROKEN SQL";
    response = server.HandleRequest(request);
    if (response.status_code != 202) return 1;
    const std::string bad_job = ExtractValue(response.payload, "job_id");
    if (bad_job.empty()) return 1;

    bool failed_job_observed = false;
    for (int i = 0; i < 100; ++i) {
        request.payload = "ASYNC RESULT " + bad_job;
        response = server.HandleRequest(request);
        if (response.status_code == 202) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        if (response.status_code != 400) return 1;
        failed_job_observed = true;
        break;
    }
    if (!failed_job_observed) return 1;

    // TELEMETRY invalid and valid command.
    request.payload = "TELEMETRY BAD";
    response = server.HandleRequest(request);
    if (response.status_code != 400) return 1;

    request.payload = "TELEMETRY SNAPSHOT";
    response = server.HandleRequest(request);
    if (response.status_code != 200) return 1;
    if (response.payload.find("current_rps=") == std::string::npos) return 1;

    // CLUSTER invalid endpoint and valid ping/list path.
    request.payload = "CLUSTER ADD_NODE bad-endpoint";
    response = server.HandleRequest(request);
    if (response.status_code != 400) return 1;

    request.payload = "CLUSTER PING";
    response = server.HandleRequest(request);
    if (response.status_code != 200) return 1;

    request.payload = "CLUSTER LIST_NODES";
    response = server.HandleRequest(request);
    if (response.status_code != 200) return 1;

    std::filesystem::remove_all(root);
    return 0;
}
