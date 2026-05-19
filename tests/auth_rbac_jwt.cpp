#include "server/entrypoint.hpp"

#include <filesystem>
#include <string>

namespace {
    std::string ExtractToken(const std::string &payload) {
        const std::string prefix = "token=";
        const auto position = payload.find(prefix);
        if (position == std::string::npos) {
            return {};
        }
        return payload.substr(position + prefix.size());
    }
}

int main() {
    const std::string root = "./test_data_auth_rbac";
    std::filesystem::remove_all(root);

    dbms::server::EntrypointServer server(root);
    dbms::network::RequestEnvelope request{
        .client_id = "auth_test",
        .jwt_token = "",
        .payload = "AUTH REGISTER admin pass admin",
    };
    auto response = server.HandleRequest(request);
    if (response.status_code != 200) return 1;

    request.payload = "AUTH REGISTER admin pass admin";
    response = server.HandleRequest(request);
    if (response.status_code != 400) return 1;

    request.payload = "CREATE DATABASE secdb;";
    response = server.HandleRequest(request);
    if (response.status_code != 401) return 1;

    request.payload = "AUTH LOGIN admin wrong";
    response = server.HandleRequest(request);
    if (response.status_code != 401) return 1;

    request.payload = "AUTH LOGIN admin pass";
    response = server.HandleRequest(request);
    if (response.status_code != 200) return 1;
    const std::string admin_token = ExtractToken(response.payload);
    if (admin_token.empty()) return 1;

    request.jwt_token = admin_token;
    request.payload = "CREATE DATABASE secdb;";
    response = server.HandleRequest(request);
    if (response.status_code != 200) return 1;

    request.jwt_token = "broken.token.value";
    request.payload = "USE secdb;";
    response = server.HandleRequest(request);
    if (response.status_code != 401) return 1;

    request.jwt_token.clear();
    request.payload = "AUTH REGISTER reader rpass reader";
    response = server.HandleRequest(request);
    if (response.status_code != 200) return 1;

    request.payload = "AUTH LOGIN reader rpass";
    response = server.HandleRequest(request);
    if (response.status_code != 200) return 1;
    const std::string reader_token = ExtractToken(response.payload);
    if (reader_token.empty()) return 1;

    request.jwt_token = reader_token;
    request.payload = "USE secdb;";
    response = server.HandleRequest(request);
    if (response.status_code == 401 || response.status_code == 403) return 1;

    request.payload = "CREATE TABLE t (id INT);";
    response = server.HandleRequest(request);
    if (response.status_code != 403) return 1;

    request.payload = "DROP DATABASE secdb;";
    response = server.HandleRequest(request);
    if (response.status_code != 403) return 1;

    request.payload = "TELEMETRY SNAPSHOT";
    response = server.HandleRequest(request);
    if (response.status_code == 401 || response.status_code == 403) return 1;

    std::filesystem::remove_all(root);
    return 0;
}
