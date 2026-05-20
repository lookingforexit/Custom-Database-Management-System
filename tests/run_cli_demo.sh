#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "$0")" && pwd)"
repo_root="$(cd "${script_dir}/.." && pwd)"

cd "${repo_root}"
rm -rf ./data
output="$(${1} --demo)"

echo "${output}" | grep -q "database created"
echo "${output}" | grep -q "table created"
echo "${output}" | grep -q "rows inserted"
echo "${output}" | grep -q "updated 1 row(s)"
echo "${output}" | grep -q "\"name\": \"Ann\""
echo "${output}" | grep -q "\"name\": \"Bob\""
