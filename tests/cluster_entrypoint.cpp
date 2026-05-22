#include "network/protocol.hpp"
#include "server/entrypoint.hpp"

#include <filesystem>
#include <iostream>
#include <optional>
#include <regex>
#include <set>
#include <string>
#include <vector>

namespace {

    int Fail(const std::string &step) {
        std::cerr << "cluster_test_failed: " << step << "\n";
        return 1;
    }

    std::optional<dbms::network::ResponseEnvelope>
    SendToNode(dbms::server::EntrypointServer &server,
               const dbms::network::RequestEnvelope &request) {
        return server.HandleRequest(request);
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

    dbms::server::EntrypointServer storage_a("./storage_data_4556",
                                             "127.0.0.1:4556");
    dbms::server::EntrypointServer storage_b("./storage_data_4557",
                                             "127.0.0.1:4557");
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
    if (response.status_code != 200 || response.payload != "pong") {
        return Fail("cluster_ping");
    }

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
        insert_sql += "(" + std::to_string(id) + ", \"n" + std::to_string(id) +
                      "\")";
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
    auto use_a = SendToNode(storage_a, node_request);
    auto use_b = SendToNode(storage_b, node_request);
    if (!use_a.has_value() || !use_b.has_value() || use_a->status_code != 200 ||
        use_b->status_code != 200) {
        return Fail("use_storage_nodes");
    }

    auto shard_a = SendToNode(
        storage_a, {.client_id = "cluster_test",
                    .jwt_token = "",
                    .payload = "SELECT * FROM t;"});
    auto shard_b = SendToNode(
        storage_b, {.client_id = "cluster_test",
                    .jwt_token = "",
                    .payload = "SELECT * FROM t;"});
    if (!shard_a.has_value() || !shard_b.has_value() ||
        shard_a->status_code != 200 || shard_b->status_code != 200) {
        return Fail("select_shard_locally");
    }
    const auto ids_a = ExtractIds(shard_a->payload);
    const auto ids_b = ExtractIds(shard_b->payload);
    if (ids_a.empty() || ids_b.empty()) return Fail("empty_distribution");
    if (static_cast<int>(ids_a.size() + ids_b.size()) != 100) {
        return Fail("distribution_total_100");
    }
    if (static_cast<int>(ids_a.size()) <= 30 ||
        static_cast<int>(ids_b.size()) <= 30) {
        return Fail("distribution_balance");
    }
    std::set<int> all_ids(ids_a.begin(), ids_a.end());
    all_ids.insert(ids_b.begin(), ids_b.end());
    if (all_ids.size() != 100) return Fail("distribution_unique");

    request.payload = "SELECT * FROM t;";
    response = server.HandleRequest(request);
    if (response.status_code != 200 || CountRows(response.payload) != 100) {
        return Fail("entry_select_all");
    }

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
    if (response.status_code != 200 || CountRows(response.payload) != 0) {
        return Fail("select_deleted_target");
    }

    dbms::server::EntrypointServer storage_c("./storage_data_4558",
                                             "127.0.0.1:4558");
    request.payload = "CLUSTER ADD_NODE 127.0.0.1:4558";
    response = server.HandleRequest(request);
    if (response.status_code != 200) return Fail("add_node_c");

    request.payload =
        "INSERT INTO t (id, name) VALUES (101, \"n101\"), (102, \"n102\"), "
        "(103, \"n103\"), (104, \"n104\"), (105, \"n105\"), (106, \"n106\");";
    response = server.HandleRequest(request);
    if (response.status_code != 200) return Fail("insert_after_add_node");

    auto shard_c = SendToNode(
        storage_c, {.client_id = "cluster_test",
                    .jwt_token = "",
                    .payload = "USE shard_db;"});
    if (!shard_c.has_value() || shard_c->status_code != 200) {
        return Fail("use_storage_node_c");
    }
    shard_c = SendToNode(
        storage_c, {.client_id = "cluster_test",
                    .jwt_token = "",
                    .payload = "SELECT * FROM t;"});
    if (!shard_c.has_value() || shard_c->status_code != 200 ||
        CountRows(shard_c->payload) == 0) {
        return Fail("distribution_to_new_node");
    }

    request.payload = "CLUSTER REMOVE_NODE 127.0.0.1:4557";
    response = server.HandleRequest(request);
    if (response.status_code != 200) return Fail("remove_node_b");
    request.payload = "CLUSTER LIST_NODES";
    response = server.HandleRequest(request);
    if (response.status_code != 200 ||
        response.payload.find("127.0.0.1:4557") != std::string::npos) {
        return Fail("list_after_remove");
    }

    request.payload = "CLUSTER ADD_NODE 127.0.0.1:49999";
    response = server.HandleRequest(request);
    if (response.status_code != 200) return Fail("add_unavailable_node");
    request.payload = "CREATE DATABASE fail_2pc;";
    response = server.HandleRequest(request);
    if (response.status_code != 502) return Fail("create_database_fail_2pc");

    std::filesystem::remove_all(root);
    std::filesystem::remove_all("./storage_data_4556");
    std::filesystem::remove_all("./storage_data_4557");
    std::filesystem::remove_all("./storage_data_4558");
    return 0;
}
