# REVERT Semantics

- `REVERT` is an autocommit operation in the current project state.
- Outside an explicit transaction, `REVERT` is autocommit.
- Inside `BEGIN ... COMMIT/ROLLBACK`, `REVERT` is transactional:
- It mutates only the session-local working state.
- Other sessions do not observe the reverted state before `COMMIT`.
- `ROLLBACK` discards the `REVERT` effect.
- `COMMIT` publishes the reverted state atomically.
- Supported forms:
- `REVERT <table> LATEST;`
- `REVERT <table> EXACT "<YYYY.MM.DD-HH:MM:SS.ffffff>";`
- `REVERT <table> AT_OR_BEFORE "<YYYY.MM.DD-HH:MM:SS.ffffff>";`
- Backward-compatible form (defaults to `AT_OR_BEFORE`):
- `REVERT <table> "<YYYY.MM.DD-HH:MM:SS.ffffff>";`
