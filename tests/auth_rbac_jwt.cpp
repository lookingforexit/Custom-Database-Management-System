#include "server/entrypoint.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

    int Fail(const std::string &step) {
        std::cerr << "auth_test_failed: " << step << "\n";
        return 1;
    }

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

    std::vector<std::string> Split(const std::string &value, char delimiter) {
        std::vector<std::string> parts;
        std::string current;
        for (char ch : value) {
            if (ch == delimiter) {
                parts.push_back(current);
                current.clear();
            } else {
                current.push_back(ch);
            }
        }
        parts.push_back(current);
        return parts;
    }

} // namespace

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
    if (response.status_code != 200) return Fail("register_admin");

    std::ifstream accounts_file(root + "/accounts.tsv");
    if (!accounts_file.is_open()) return Fail("open_accounts_file");
    std::string accounts_content((std::istreambuf_iterator<char>(accounts_file)),
                                 std::istreambuf_iterator<char>());
    if (accounts_content.find("\tpass\t") != std::string::npos ||
        accounts_content.find("\tpass\n") != std::string::npos ||
        accounts_content.find(" password=pass") != std::string::npos) {
        return Fail("plaintext_password");
    }
    if (accounts_content.find("salt=") == std::string::npos) return Fail("missing_salt");
    if (accounts_content.find("password_hash=") == std::string::npos) return Fail("missing_password_hash");

    request.payload = "AUTH REGISTER admin pass admin";
    response = server.HandleRequest(request);
    if (response.status_code != 400) return Fail("duplicate_register_admin");

    request.payload = "CREATE DATABASE secdb;";
    response = server.HandleRequest(request);
    if (response.status_code != 401) return Fail("unauth_create_db");

    request.payload = "AUTH LOGIN admin wrong";
    response = server.HandleRequest(request);
    if (response.status_code != 401) return Fail("login_wrong_password");

    request.payload = "AUTH LOGIN admin pass";
    response = server.HandleRequest(request);
    if (response.status_code != 200) return Fail("login_admin");
    const std::string admin_token = ExtractValue(response.payload, "token");
    if (admin_token.empty()) return Fail("extract_admin_token");
    const auto admin_parts = Split(admin_token, '.');
    if (admin_parts.size() != 3) return Fail("jwt_parts");

    request.jwt_token = admin_token;
    request.payload = "AUTH CREATE_GROUP writers";
    response = server.HandleRequest(request);
    if (response.status_code != 200) return Fail("create_group_writers");

    request.payload = "CREATE DATABASE secdb;";
    response = server.HandleRequest(request);
    if (response.status_code != 200) return Fail("create_secdb");

    request.payload = "USE secdb;";
    response = server.HandleRequest(request);
    if (response.status_code != 200) return Fail("use_secdb_admin");

    request.payload = "CREATE TABLE t (id INT INDEXED, name STRING);";
    response = server.HandleRequest(request);
    if (response.status_code != 200) return Fail("create_table_admin");

    request.jwt_token.clear();
    request.payload = "AUTH REGISTER reader rpass reader";
    response = server.HandleRequest(request);
    if (response.status_code != 200) return Fail("register_reader");

    request.payload = "AUTH REGISTER writer wpass";
    response = server.HandleRequest(request);
    if (response.status_code != 200) return Fail("register_writer");

    request.jwt_token = admin_token;
    request.payload = "AUTH ADD_USER_GROUP writer writers";
    response = server.HandleRequest(request);
    if (response.status_code != 200) return Fail("add_writer_group");

    request.payload = "AUTH GRANT_DEFAULT secdb READ";
    response = server.HandleRequest(request);
    if (response.status_code != 200) return Fail("grant_default_read");

    request.payload = "AUTH GRANT_GROUP secdb writers WRITE";
    response = server.HandleRequest(request);
    if (response.status_code != 200) return Fail("grant_group_write");

    request.payload = "AUTH GRANT_USER secdb writer DROP_TABLE";
    response = server.HandleRequest(request);
    if (response.status_code != 200) return Fail("grant_user_drop_table");

    request.jwt_token.clear();
    request.payload = "AUTH LOGIN reader rpass";
    response = server.HandleRequest(request);
    if (response.status_code != 200) return Fail("login_reader");
    const std::string reader_token = ExtractValue(response.payload, "token");
    if (reader_token.empty()) return Fail("extract_reader_token");

    request.jwt_token = reader_token;
    request.payload = "USE secdb;";
    response = server.HandleRequest(request);
    if (response.status_code != 200) return Fail("use_secdb_reader");
    request.payload = "SELECT * FROM t;";
    response = server.HandleRequest(request);
    if (response.status_code != 200) return Fail("select_reader");
    request.payload = "INSERT INTO t (id, name) VALUES (1, \"A\");";
    response = server.HandleRequest(request);
    if (response.status_code != 403) return Fail("reader_insert_denied");

    request.jwt_token.clear();
    request.payload = "AUTH LOGIN writer wpass";
    response = server.HandleRequest(request);
    if (response.status_code != 200) return Fail("login_writer");
    const std::string writer_token = ExtractValue(response.payload, "token");
    if (writer_token.empty()) return Fail("extract_writer_token");

    request.jwt_token = writer_token;
    request.payload = "USE secdb;";
    response = server.HandleRequest(request);
    if (response.status_code != 200) return Fail("use_secdb_writer");
    request.payload = "INSERT INTO t (id, name) VALUES (1, \"A\");";
    response = server.HandleRequest(request);
    if (response.status_code != 200) return Fail("writer_insert");
    request.payload = "UPDATE t SET name = \"B\" WHERE id == 1;";
    response = server.HandleRequest(request);
    if (response.status_code != 200) return Fail("writer_update");
    request.payload = "DROP TABLE t;";
    response = server.HandleRequest(request);
    if (response.status_code != 200) return Fail("writer_drop_table_user_grant");

    request.jwt_token = admin_token;
    request.payload = "CREATE TABLE t (id INT INDEXED, name STRING);";
    response = server.HandleRequest(request);
    if (response.status_code != 200) return Fail("recreate_table_admin");
    request.payload = "AUTH REVOKE_USER secdb writer DROP_TABLE";
    response = server.HandleRequest(request);
    if (response.status_code != 200) return Fail("revoke_user_drop_table");

    request.jwt_token = writer_token;
    request.payload = "DROP TABLE t;";
    response = server.HandleRequest(request);
    if (response.status_code != 403) return Fail("writer_drop_table_revoked");

    request.jwt_token = admin_token;
    request.payload = "AUTH WHOAMI";
    response = server.HandleRequest(request);
    if (response.status_code != 200 ||
        response.payload.find("user=admin") == std::string::npos) {
        return Fail("whoami_admin");
    }

    request.jwt_token = "broken.token.value";
    request.payload = "USE secdb;";
    response = server.HandleRequest(request);
    if (response.status_code != 401) return Fail("bad_token_rejected");

    request.jwt_token = admin_token;
    request.jwt_token.back() = request.jwt_token.back() == 'A' ? 'B' : 'A';
    request.payload = "USE secdb;";
    response = server.HandleRequest(request);
    if (response.status_code != 401) return Fail("tampered_token_rejected");

    {
        dbms::server::EntrypointServer reloaded(root);
        dbms::network::RequestEnvelope reload_request{
            .client_id = "reload",
            .jwt_token = "",
            .payload = "AUTH LOGIN writer wpass",
        };
        response = reloaded.HandleRequest(reload_request);
        if (response.status_code != 200) return Fail("reload_login_writer");
        const std::string reloaded_token = ExtractValue(response.payload, "token");
        if (reloaded_token.empty()) return Fail("reload_extract_token");

        reload_request.jwt_token = reloaded_token;
        reload_request.payload = "USE secdb;";
        response = reloaded.HandleRequest(reload_request);
        if (response.status_code != 200) return Fail("reload_use_secdb");
        reload_request.payload = "INSERT INTO t (id, name) VALUES (2, \"C\");";
        response = reloaded.HandleRequest(reload_request);
        if (response.status_code != 200) return Fail("reload_writer_insert");
        reload_request.payload = "DROP TABLE t;";
        response = reloaded.HandleRequest(reload_request);
        if (response.status_code != 403) return Fail("reload_writer_drop_denied");
    }

    std::filesystem::remove_all(root);
    return 0;
}
