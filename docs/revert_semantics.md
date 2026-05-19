# REVERT Semantics

- `REVERT` is an autocommit operation in the current project state.
- Transactional commands (`BEGIN/COMMIT/ROLLBACK`) are not implemented yet.
- Because of that, `REVERT` is always executed immediately and persisted as a new snapshot.
- Supported forms:
- `REVERT <table> LATEST;`
- `REVERT <table> EXACT "<YYYY.MM.DD-HH:MM:SS.ffffff>";`
- `REVERT <table> AT_OR_BEFORE "<YYYY.MM.DD-HH:MM:SS.ffffff>";`
- Backward-compatible form (defaults to `AT_OR_BEFORE`):
- `REVERT <table> "<YYYY.MM.DD-HH:MM:SS.ffffff>";`
