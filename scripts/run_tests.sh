#!/usr/bin/env bash
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
PAWELI="${REPO_ROOT}/build/debug/paweli"

if [ ! -x "${PAWELI}" ]; then
	echo "paweli binary not found at ${PAWELI} -- run 'make build' first" >&2
	exit 1
fi

pass=0
fail=0

for pw_file in "${REPO_ROOT}"/tests/*.pw; do
	name=$(basename "${pw_file}")
	output=$("${PAWELI}" "${pw_file}" 2>&1)
	code=$?

	if [ "${code}" -eq 0 ]; then
		echo "PASS ${name}"
		pass=$((pass + 1))
	else
		echo "FAIL ${name}"
		echo "${output}" | sed 's/^/    /'
		fail=$((fail + 1))
	fi
done

echo "──────────────────────────────"
echo "${pass} passed, ${fail} failed"

if [ "${fail}" -gt 0 ]; then
	exit 1
fi
