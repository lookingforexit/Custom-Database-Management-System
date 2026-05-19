#include "server/entrypoint.hpp"

#include <chrono>
#include <filesystem>
#include <thread>

namespace {
    std::string ExtractJobId(const std::string &payload) {
        const std::string prefix = "job_id=";
        const auto position = payload.find(prefix);
        if (position == std::string::npos) {
            return {};
        }
        return payload.substr(position + prefix.size());
    }
}

int main() {
    const std::string root_path = "./test_data_async_queue";
    std::filesystem::remove_all(root_path);
    dbms::server::EntrypointServer server(root_path);
    dbms::network::RequestEnvelope request{
        .client_id = "async_test",
        .jwt_token = "",
        .payload = "ASYNC SUBMIT CREATE DATABASE adb",
    };

    auto response = server.HandleRequest(request);
    if (response.status_code != 202) return 1;
    const std::string job_id = ExtractJobId(response.payload);
    if (job_id.empty()) return 1;

    request.payload = "ASYNC STATUS " + job_id;
    response = server.HandleRequest(request);
    if (response.status_code != 200) return 1;

    request.payload = "ASYNC STATUS missing-id";
    response = server.HandleRequest(request);
    if (response.status_code != 404) return 1;

    bool done = false;
    for (int attempt = 0; attempt < 40; ++attempt) {
        request.payload = "ASYNC RESULT " + job_id;
        response = server.HandleRequest(request);
        if (response.status_code == 202) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        if (response.status_code != 200) return 1;
        done = true;
        break;
    }
    if (!done) return 1;

    // Invalid async command forms.
    request.payload = "ASYNC SUBMIT";
    response = server.HandleRequest(request);
    if (response.status_code != 400) return 1;

    request.payload = "ASYNC RESULT missing-id";
    response = server.HandleRequest(request);
    if (response.status_code != 404) return 1;

    // Failed async job must settle with non-202 status.
    request.payload = "ASYNC SUBMIT BROKEN SQL";
    response = server.HandleRequest(request);
    if (response.status_code != 202) return 1;
    const std::string bad_job_id = ExtractJobId(response.payload);
    if (bad_job_id.empty()) return 1;
    bool failed = false;
    for (int attempt = 0; attempt < 60; ++attempt) {
        request.payload = "ASYNC RESULT " + bad_job_id;
        response = server.HandleRequest(request);
        if (response.status_code == 202) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        if (response.status_code != 400) return 1;
        failed = true;
        break;
    }
    if (!failed) return 1;

    return 0;
}
