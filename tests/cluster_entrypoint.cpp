#include "network/protocol.hpp"
#include "server/entrypoint.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <csignal>
#include <filesystem>
#include <iostream>
#include <optional>
#include <regex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {

    int Fail(const std::string &step) {
        std::cerr << "cluster_test_failed: " << step << "\n";
        return 1;
    }

    bool ReadLine(int socket_fd, std::string &line) {
        line.clear();
        char ch = '\0';
        while (true) {
            const auto bytes = recv(socket_fd, &ch, 1, 0);
            if (bytes <= 0) {
                return false;
            }
            if (ch == '\n') {
                return true;
            }
            line.push_back(ch);
        }
    }

    bool WriteAll(int socket_fd, const std::string &buffer) {
        std::size_t sent = 0;
        while (sent < buffer.size()) {
            const auto bytes =
                send(socket_fd, buffer.data() + static_cast<long>(sent),
                     buffer.size() - sent, 0);
            if (bytes <= 0) {
                return false;
            }
            sent += static_cast<std::size_t>(bytes);
        }
        return true;
    }

    std::optional<dbms::network::ResponseEnvelope>
    SendToPort(int port, const dbms::network::RequestEnvelope &request) {
        const int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (socket_fd < 0) {
            return std::nullopt;
        }
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(static_cast<uint16_t>(port));
        if (inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) != 1) {
            close(socket_fd);
            return std::nullopt;
        }
        if (connect(socket_fd, reinterpret_cast<sockaddr *>(&address),
                    sizeof(address)) != 0) {
            close(socket_fd);
            return std::nullopt;
        }
        if (!WriteAll(socket_fd, dbms::network::SerializeRequest(request))) {
            close(socket_fd);
            return std::nullopt;
        }
        std::string wire_response;
        if (!ReadLine(socket_fd, wire_response)) {
            close(socket_fd);
            return std::nullopt;
        }
        close(socket_fd);
        dbms::network::ResponseEnvelope response;
        if (!dbms::network::DeserializeResponse(wire_response, response)) {
            return std::nullopt;
        }
        return response;
    }

    pid_t SpawnStorageNode(int port) {
        std::filesystem::path executable = "./dbms_storage_node";
        if (!std::filesystem::exists(executable)) {
            executable = "./build/dbms_storage_node";
        }
        const pid_t pid = fork();
        if (pid != 0) {
            return pid;
        }
        execl(executable.c_str(), executable.c_str(),
              std::to_string(port).c_str(), nullptr);
        _exit(1);
    }

    bool WaitForPort(int port) {
        for (int attempt = 0; attempt < 40; ++attempt) {
            auto response = SendToPort(
                port, {.client_id = "probe", .jwt_token = "", .payload = "CLUSTER PING"});
            if (response.has_value() && response->status_code == 200 &&
                response->payload == "pong") {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        return false;
    }

    std::vector<int> ExtractIds(const std::string &json) {
        std::vector<int> ids;
        std::regex id_re(R"(\"id\":\s*(-?\d+))");
        for (std::sregex_iterator it(json.begin(), json.end(), id_re), end;
             it != end; ++it) {
            ids.push_back(std::stoi((*it)[1].str()));
        }
        return ids;
    }

    int CountRows(const std::string &json) {
        return static_cast<int>(ExtractIds(json).size());
    }

} // namespace

int main() {
    const std::string root = "./test_data_cluster_entrypoint";
    std::filesystem::remove_all(root);
    std::filesystem::remove_all("./storage_data_4556");
    std::filesystem::remove_all("./storage_data_4557");
    std::filesystem::remove_all("./storage_data_4558");

    const pid_t node_a = SpawnStorageNode(4556);
    const pid_t node_b = SpawnStorageNode(4557);
    if (node_a <= 0 || node_b <= 0) return Fail("spawn_initial_nodes");
    if (!WaitForPort(4556) || !WaitForPort(4557)) return Fail("wait_initial_nodes");

    dbms::server::EntrypointServer server(root);
    dbms::network::RequestEnvelope request{
        .client_id = "cluster",
        .jwt_token = "",
        .payload = "CLUSTER LIST_NODES",
    };
    auto response = server.HandleRequest(request);
    if (response.status_code != 200) return Fail("list_nodes_initial");

    request.payload = "CLUSTER PING";
    response = server.HandleRequest(request);
    if (response.status_code != 200 || response.payload != "pong") return Fail("cluster_ping");

    request.payload = "CLUSTER PREPARE_TX tx_local_1 CREATE DATABASE prepared_db;";
    response = server.HandleRequest(request);
    if (response.status_code != 200) return Fail("prepare_local_tx");
    request.payload = "CLUSTER COMMIT_TX tx_local_1";
    response = server.HandleRequest(request);
    if (response.status_code != 200) return Fail("commit_local_tx");
    request.payload = "USE prepared_db;";
    response = server.HandleRequest(request);
    if (response.status_code != 200) return Fail("use_prepared_db");

    request.payload = "CLUSTER ADD_NODE 127.0.0.1:4556";
    response = server.HandleRequest(request);
    if (response.status_code != 200) return Fail("add_node_a");
    request.payload = "CLUSTER ADD_NODE 127.0.0.1:4557";
    response = server.HandleRequest(request);
    if (response.status_code != 200) return Fail("add_node_b");

    request.payload = "CREATE DATABASE shard_db;";
    response = server.HandleRequest(request);
    if (response.status_code != 200) return Fail("create_shard_db");
    request.payload = "USE shard_db;";
    response = server.HandleRequest(request);
    if (response.status_code != 200) return Fail("use_shard_db");
    request.payload = "CREATE TABLE t (id INT INDEXED, name STRING);";
    response = server.HandleRequest(request);
    if (response.status_code != 200) return Fail("create_table");

    std::string insert_sql = "INSERT INTO t (id, name) VALUES ";
    for (int id = 1; id <= 100; ++id) {
        if (id != 1) insert_sql += ", ";
        insert_sql += "(" + std::to_string(id) + ", \"n" + std::to_string(id) + "\")";
    }
    insert_sql += ";";
    request.payload = insert_sql;
    response = server.HandleRequest(request);
    if (response.status_code != 200) return Fail("insert_1_100");

    const dbms::network::RequestEnvelope node_request{
        .client_id = "cluster_test",
        .jwt_token = "",
        .payload = "USE shard_db;",
    };
    auto use_a = SendToPort(4556, node_request);
    auto use_b = SendToPort(4557, node_request);
    if (!use_a.has_value() || !use_b.has_value() || use_a->status_code != 200 ||
        use_b->status_code != 200) {
        return Fail("use_storage_nodes");
    }

    auto shard_a = SendToPort(
        4556, {.client_id = "cluster_test", .jwt_token = "", .payload = "SELECT * FROM t;"});
    auto shard_b = SendToPort(
        4557, {.client_id = "cluster_test", .jwt_token = "", .payload = "SELECT * FROM t;"});
    if (!shard_a.has_value() || !shard_b.has_value() ||
        shard_a->status_code != 200 || shard_b->status_code != 200) {
        return Fail("select_shard_locally");
    }
    const auto ids_a = ExtractIds(shard_a->payload);
    const auto ids_b = ExtractIds(shard_b->payload);
    if (ids_a.empty() || ids_b.empty()) return Fail("empty_distribution");
    if (static_cast<int>(ids_a.size() + ids_b.size()) != 100) return Fail("distribution_total_100");
    if (static_cast<int>(ids_a.size()) <= 30 ||
        static_cast<int>(ids_b.size()) <= 30) {
        return Fail("distribution_balance");
    }
    std::set<int> all_ids(ids_a.begin(), ids_a.end());
    all_ids.insert(ids_b.begin(), ids_b.end());
    if (all_ids.size() != 100) return Fail("distribution_unique");

    request.payload = "SELECT * FROM t;";
    response = server.HandleRequest(request);
    if (response.status_code != 200 || CountRows(response.payload) != 100) return Fail("entry_select_all");

    request.payload = "SELECT * FROM t WHERE id == 42;";
    response = server.HandleRequest(request);
    if (response.status_code != 200 || CountRows(response.payload) != 1 ||
        response.payload.find("\"id\": 42") == std::string::npos) {
        return Fail("entry_select_targeted");
    }

    request.payload = "UPDATE t SET name = \"updated\" WHERE id == 42;";
    response = server.HandleRequest(request);
    if (response.status_code != 200) return Fail("update_targeted");
    request.payload = "SELECT name FROM t WHERE id == 42;";
    response = server.HandleRequest(request);
    if (response.status_code != 200 ||
        response.payload.find("\"name\": \"updated\"") == std::string::npos) {
        return Fail("select_updated_target");
    }

    request.payload = "DELETE FROM t WHERE id == 42;";
    response = server.HandleRequest(request);
    if (response.status_code != 200) return Fail("delete_targeted");
    request.payload = "SELECT * FROM t WHERE id == 42;";
    response = server.HandleRequest(request);
    if (response.status_code != 200 || CountRows(response.payload) != 0) return Fail("select_deleted_target");

    const pid_t node_c = SpawnStorageNode(4558);
    if (node_c <= 0 || !WaitForPort(4558)) return Fail("spawn_node_c");
    request.payload = "CLUSTER ADD_NODE 127.0.0.1:4558";
    response = server.HandleRequest(request);
    if (response.status_code != 200) return Fail("add_node_c");

    std::string insert_more = "INSERT INTO t (id, name) VALUES ";
    for (int id = 101; id <= 130; ++id) {
        if (id != 101) insert_more += ", ";
        insert_more += "(" + std::to_string(id) + ", \"n" + std::to_string(id) + "\")";
    }
    insert_more += ";";
    request.payload = insert_more;
    response = server.HandleRequest(request);
    if (response.status_code != 200) return Fail("insert_101_130");

    auto use_c = SendToPort(
        4558, {.client_id = "cluster_test", .jwt_token = "", .payload = "USE shard_db;"});
    if (!use_c.has_value() || use_c->status_code != 200) return Fail("use_node_c");
    auto shard_c = SendToPort(
        4558, {.client_id = "cluster_test", .jwt_token = "", .payload = "SELECT * FROM t;"});
    if (!shard_c.has_value() || shard_c->status_code != 200) return Fail("select_node_c");
    if (CountRows(shard_c->payload) == 0) return Fail("node_c_received_rows");

    request.payload = "CLUSTER REMOVE_NODE 127.0.0.1:4558";
    response = server.HandleRequest(request);
    if (response.status_code != 200) return Fail("remove_node_c");

    request.payload = "CLUSTER LIST_NODES";
    response = server.HandleRequest(request);
    if (response.status_code != 200 ||
        response.payload.find("nodes=2") == std::string::npos) {
        return Fail("list_nodes_after_remove_c");
    }

    request.payload = "CLUSTER REMOVE_NODE 127.0.0.1:4556";
    response = server.HandleRequest(request);
    if (response.status_code != 200) return Fail("remove_node_a");
    request.payload = "CLUSTER REMOVE_NODE 127.0.0.1:4557";
    response = server.HandleRequest(request);
    if (response.status_code != 200) return Fail("remove_node_b");

    request.payload = "CLUSTER ADD_NODE 127.0.0.1:49999";
    response = server.HandleRequest(request);
    if (response.status_code != 200) return Fail("add_unavailable_node");
    request.payload = "CREATE DATABASE fail_2pc;";
    response = server.HandleRequest(request);
    if (response.status_code != 502) return Fail("2pc_fail_path");
    request.payload = "CLUSTER REMOVE_NODE 127.0.0.1:49999";
    response = server.HandleRequest(request);
    if (response.status_code != 200) return Fail("remove_unavailable_node");

    kill(node_a, SIGTERM);
    kill(node_b, SIGTERM);
    kill(node_c, SIGTERM);
    waitpid(node_a, nullptr, 0);
    waitpid(node_b, nullptr, 0);
    waitpid(node_c, nullptr, 0);

    std::filesystem::remove_all(root);
    std::filesystem::remove_all("./storage_data_4556");
    std::filesystem::remove_all("./storage_data_4557");
    std::filesystem::remove_all("./storage_data_4558");
    return 0;
}
