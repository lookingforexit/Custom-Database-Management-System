# REVERT Storage Audit

This project implements `REVERT` via logical history records, not by copying full DB files.

## Storage model

- Current state is stored in `runtime_state.tsv`.
- Temporal history is stored in `version_history.tsv`.
- Each history item stores:
  - `db`
  - `table`
  - `operation`
  - `timestamp`
  - `SNAPSHOT_ROW` entries (row values only)

## What is explicitly not used

- No full filesystem snapshot per timestamp.
- No table-file clone per timestamp.
- No copy of `runtime_state.tsv` as point-in-time backups during regular `REVERT`.

`REVERT` resolves a history record by timestamp/mode and reconstructs table state from row snapshots + index rebuild.
