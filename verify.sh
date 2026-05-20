#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'USAGE'
Usage: ./verify.sh [--dry-run] [--build-dir <dir>] [--skip-configure]

Modes:
  default     Run full one-shot verification.
  --dry-run   Print commands without executing.

Steps (one-shot):
  1) cmake configure
  2) cmake build
  3) ctest full suite
  4) cli batch script
  5) cli demo script
USAGE
}

DRY_RUN=0
BUILD_DIR="build"
SKIP_CONFIGURE=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --dry-run)
      DRY_RUN=1
      shift
      ;;
    --build-dir)
      BUILD_DIR="${2:-}"
      if [[ -z "$BUILD_DIR" ]]; then
        echo "error: --build-dir requires value" >&2
        exit 2
      fi
      shift 2
      ;;
    --skip-configure)
      SKIP_CONFIGURE=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "error: unknown argument: $1" >&2
      usage
      exit 2
      ;;
  esac
done

run_cmd() {
  local cmd="$1"
  if [[ "$DRY_RUN" -eq 1 ]]; then
    printf '[dry-run] %s\n' "$cmd"
  else
    printf '[run] %s\n' "$cmd"
    bash -lc "$cmd"
  fi
}

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$SCRIPT_DIR"

if [[ "$SKIP_CONFIGURE" -eq 0 ]]; then
  run_cmd "cmake -S '$REPO_ROOT' -B '$REPO_ROOT/$BUILD_DIR'"
fi
run_cmd "cmake --build '$REPO_ROOT/$BUILD_DIR' -j"
run_cmd "ctest --test-dir '$REPO_ROOT/$BUILD_DIR' --output-on-failure"
run_cmd "'$REPO_ROOT/tests/run_cli_batch.sh' '$REPO_ROOT/$BUILD_DIR/dbms_cli'"
run_cmd "'$REPO_ROOT/tests/run_cli_demo.sh' '$REPO_ROOT/$BUILD_DIR/dbms_cli'"

if [[ "$DRY_RUN" -eq 1 ]]; then
  echo "verify: dry-run complete"
else
  echo "verify: one-shot complete"
fi
