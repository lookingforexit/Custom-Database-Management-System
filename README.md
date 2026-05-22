# Custom Database Management System (CW2026)

Учебная СУБД на C++20 с SQL-подобным языком, персистентностью, индексами, `REVERT`, транзакциями, клиент-серверным режимом, шардированным кластером и расширенным тестовым контуром.

## 1. Что это за проект

Проект реализует:
- SQL-ядро (DDL/DML/SELECT/WHERE/агрегаты),
- B\*+\-tree индекс для `INDEXED` колонок,
- in-memory runtime + файловая персистентность,
- delta/event history для temporal `REVERT`,
- транзакции с изоляцией по сессии,
- единый error contract,
- TCP server + remote CLI,
- кластерный entrypoint + storage nodes + row-level sharding,
- heartbeat/restart managed node,
- async jobs,
- access logs и telemetry,
- RBAC/auth/JWT (HS256),
- WAL с recovery (redo/undo + corrupted WAL quarantine),
- сервисные команды `CHECK INDEX` / `REBUILD INDEX` на уровне SQL AST,
- coordinator-side 2PC для metadata и cross-node mutating paths,
- one-shot верификация через `verify.sh` (`--dry-run` поддерживается).

## 2. Архитектура

Основные модули:
- `src/parser` — lexer/parser/AST SQL-подобного языка.
- `src/planner` — план выполнения.
- `src/execution` — выполнение DDL/DML/SELECT/REVERT/tx.
- `src/index` — канонический B\*+\-tree.
- `src/core` — `DbmsEngine`, runtime state, persistence, WAL и session-scoped транзакции (без отдельного `transaction` модуля).
- `src/versioning` — append-only delta history для `REVERT`.
- `src/storage` — `TableHeap`, `StringPool`, interned string references.
- `src/server` + `src/network` — entrypoint и TCP протокол.
- `apps/cli` — локальный/удаленный CLI.
- `apps/server` — entrypoint server.
- `apps/storage_node` — storage node процесс.

## 3. Реализация требований

### Базовые требования

1. SQL-ядро: `CREATE/DROP DATABASE`, `USE`, `CREATE/DROP TABLE`, `INSERT`, `UPDATE`, `DELETE`, `SELECT`, `WHERE`.
- Реализация: `src/parser`, `src/planner`, `src/execution`.
- Проверка: `tests/smoke.cpp`, `tests/core_integration_extended.cpp`.

2. Фильтры и выражения: `AND/OR`, скобки, `BETWEEN`, `LIKE`, сравнения.
- Реализация: `src/parser/parser.cpp`, `src/execution/execution_engine.cpp`.
- Проверка: `tests/parser_validation.cpp`, `tests/where_and_or_stress.cpp`.

3. Агрегаты: `COUNT`, `SUM`, `AVG`.
- Реализация: `src/execution/execution_engine.cpp`.
- Проверка: `tests/aggregate_edge_cases.cpp`, `tests/core_integration_extended.cpp`.

4. Индексы (B\*+\-tree) и индексная консистентность.
- Реализация: `src/index/b_star_plus_tree.*`, `src/core/runtime_state.*`.
- Проверка: `tests/bstar_plus_tree.cpp`, `tests/bstar_plus_tree_stress.cpp`, `tests/index_consistency.cpp`, `tests/index_no_data_duplication.cpp`.

5. Персистентность runtime/history/WAL.
- Реализация: `src/core/runtime_persistence.*`.
- Проверка: `tests/persistence.cpp`, `tests/revert_persistence.cpp`.

6. `REVERT` (LATEST/EXACT/AT_OR_BEFORE и короткая форма `REVERT table "timestamp";`) без snapshot-копий таблиц/файлов.
- Реализация: `src/execution/execution_engine.cpp`, `src/versioning/version_store.cpp`.
- Проверка: `tests/revert.cpp`, `tests/revert_requirements.cpp`, `tests/revert_no_file_snapshot.cpp`, `tests/revert_transactions.cpp`.

7. Транзакции (`BEGIN/COMMIT/ROLLBACK`) с изоляцией по сессии.
- Реализация: `src/core/dbms_engine.cpp`.
- Проверка: `tests/transactions.cpp`, `tests/revert_transactions.cpp`.

8. Единый контракт ошибок.
- Реализация: `src/common/error_contract.*`.
- Проверка: `tests/error_contract.cpp`.

### Дополнительные требования

1. Revert-аудит — закрыт.
2. String interning/dedup — закрыт (`src/storage/string_pool.*`, `src/common/types.*`, `tests/string_interning.cpp`).
3. Клиент-сервер — закрыт (`apps/server`, `apps/cli`, `src/network/protocol.*`).
4. Кластер + шардирование + динамика узлов — закрыт (`src/server/entrypoint.*`, `apps/storage_node`, `tests/cluster_entrypoint.cpp`).
5. Heartbeat + restart — закрыт (`RunHeartbeatCycle`, managed nodes).
6. Async очередь + GUID/status API — закрыт (`src/runtime/job_queue.*`, `ASYNC ...`).
7. Access logs — закрыт (`access.log`, `tests/access_logs.cpp`).
8. Telemetry realtime — закрыт (`src/runtime/telemetry.*`, `TELEMETRY LOCAL`, `TELEMETRY SNAPSHOT`).
9. RBAC + auth + JWT + salted hashes — закрыт (`src/catalog/rbac.*`, `AUTH REGISTER/LOGIN/GRANT_*/REVOKE_*`).
10. DEFAULT acceptance — закрыт (`tests/default_acceptance.cpp`).
11. WHERE AND/OR stress — закрыт (`tests/where_and_or_stress.cpp`).
12. SUM/COUNT/AVG edge — закрыт (`tests/aggregate_edge_cases.cpp`).

## 4. Форматы данных

### `runtime_state.tsv` (human-readable)
- Блоки `DATABASE`, `TABLE`, `COLUMN`, `ROW`.
- Значения: `Null`, `Int:<num>`, `String:<text>`.
- Строковые значения сохраняются как обычный текст, а при загрузке снова интернируются через `StringPool`.

### `version_history.tsv` (FORMAT 3)
- Append-only event log, без `SNAPSHOT_ROW`.
- События включают:
  - `CREATE_DATABASE`
  - `DROP_DATABASE`
  - `CREATE_TABLE`
  - `DROP_TABLE`
  - `INSERT_ROW`
  - `UPDATE_ROW`
  - `DELETE_ROW`
- Для `UPDATE_ROW` и `DELETE_ROW` хранится `before`-состояние строки.
- Для `INSERT_ROW` хранится вставленная строка, чтобы rollback/replay могли ее удалить или восстановить.
- История использует стабильные `row_id`, поэтому `REVERT` не зависит от перестроения heap/index.

### `wal.log` (v2)
- Заголовок: `WAL\t2`.
- Записи:
  - `SQL\t...` — одиночная мутация.
  - `TX_BEGIN` / `TX_SQL\t...` / `TX_END` — атомарный блок commit-транзакции.
- Recovery:
  - `redo` для полных записей,
  - `undo` для оборванного `TX_*` хвоста,
  - quarantine для битого/неподдерживаемого WAL (`wal.log.corrupt.<ts>`).

## 5. Сервисные команды

Доступны напрямую в SQL-потоке:
- `CHECK INDEX;` — валидирует инварианты индексов + heap/index соответствие.
- `REBUILD INDEX;` — пересборка индексов из heap и повторная валидация.

Ограничение:
- Внутри активной транзакции сервисные index-команды запрещены.

## 6. Сборка

Требования:
- CMake >= 3.20
- C++20 compiler (проверялось с GCC 13.x)
- OpenSSL (libcrypto для JWT HS256)

Команды:
```bash
cmake -S . -B build
cmake --build build -j4
```

## 7. Запуск

### Локальный CLI
```bash
./build/dbms_cli
./build/dbms_cli script.sql
./build/dbms_cli --demo
./build/dbms_cli --help
```

### Сервер + remote CLI
```bash
./build/dbms_server 4545
./build/dbms_cli --server 127.0.0.1:4545
./build/dbms_cli --server 127.0.0.1:4545 script.sql
./build/dbms_cli --server 127.0.0.1:4545 --jwt <token> --demo
```

### Storage node
```bash
./build/dbms_storage_node 4546
```

## 8. Проверка тестами

One-shot проверка:
```bash
./verify.sh
./verify.sh --dry-run
```

Эквивалент вручную:
```bash
cmake -S . -B build
cmake --build build -j4
ctest --test-dir build --output-on-failure
```

Полный прогон:
```bash
ctest --test-dir build --output-on-failure
```

Примечание по sandbox:
- `dbms_cluster_entrypoint` теперь проходит и в sandbox-режиме.
- Для этого тест использует in-process transport registry для healthy storage nodes, но production TCP path в `EntrypointServer` сохранен и продолжает использоваться как fallback.

Ключевые группы:
```bash
ctest --test-dir build -R "dbms_wal_recovery|dbms_index_service|dbms_transactions" --output-on-failure
ctest --test-dir build -R "dbms_cli_batch|dbms_cli_demo" --output-on-failure
ctest --test-dir build -R "dbms_where_and_or_stress|dbms_aggregate_edge_cases" --output-on-failure
```

## 9. Практические фичи

- `AUTH REGISTER/LOGIN/WHOAMI`, `AUTH CREATE_GROUP`, `AUTH ADD_USER_GROUP`.
- `AUTH GRANT_DEFAULT/REVOKE_DEFAULT`, `AUTH GRANT_GROUP/REVOKE_GROUP`, `AUTH GRANT_USER/REVOKE_USER`.
- JWT HS256 (`header.payload.signature`, claims `sub/exp`) с persistent secret в data root.
- `ASYNC SUBMIT/STATUS/RESULT`.
- `TELEMETRY LOCAL` и cluster-level `TELEMETRY SNAPSHOT`.
- `CLUSTER ADD_NODE/REMOVE_NODE/LIST_NODES/PING`.
- `CLUSTER PREPARE_TX/COMMIT_TX/ABORT_TX`.
- Row-level sharding:
  - `INSERT` режется по shard key и отправляется ровно на один storage node для каждой строки.
  - `SELECT/UPDATE/DELETE` с equality по shard key маршрутизируются адресно.
  - fan-out используется только когда shard key из запроса вывести нельзя.
- Heartbeat и restart managed node.
- Access log с `start`, `finish`, `client_id`, `handler_id`, `status_code`, `latency_ms`, `sql`.

## 10. Быстрый чек-лист “проект в порядке”

1. `./verify.sh`
2. (или вручную) `cmake -S . -B build && cmake --build build -j4`
3. `ctest --test-dir build --output-on-failure`
4. `./build/dbms_cli --demo`
5. (опционально) `./build/dbms_server 4545` + remote demo через `dbms_cli --server ...`
