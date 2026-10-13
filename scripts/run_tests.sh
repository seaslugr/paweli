#!/usr/bin/env bash
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
SUITE="${REPO_ROOT}/tests/suite/main.pw"

# Three overrides let this script test a cross-compiled Windows build
# under wine (see `make test-windows`):
#   PAWELI             the binary to test
#   PAWELI_RUNNER      a command that runs it, e.g. "wine"
#   PAWELI_EXE_SUFFIX  what `build --bin` adds to its output, e.g. ".exe"
PAWELI="${PAWELI:-${REPO_ROOT}/build/debug/paweli}"
RUNNER="${PAWELI_RUNNER:-}"
EXE_SUFFIX="${PAWELI_EXE_SUFFIX:-}"

# Runs a paweli binary or a built executable with stdin closed and
# stderr folded into stdout. Strips the carriage returns a Windows
# build writes, so output compares the same on every platform.
run_exe() {
	${RUNNER} "$@" < /dev/null 2>&1 | tr -d '\r'
	return "${PIPESTATUS[0]}"
}

if [ ! -x "${PAWELI}" ]; then
	echo "paweli binary not found at ${PAWELI}. Run 'make build' first" >&2
	exit 1
fi

# A debug tripwire. paweli aborts if JIT code allocates for too long
# without reaching a GC safepoint (see gc_heap_note_alloc() in gc.c).
# A JIT call site that allocates with no GC check then fails the
# suite at once.
export PAWELI_GC_SAFEPOINT_CHECK=1

pass=0
fail=0

# The suite is one program: tests/suite/main.pw imports one module
# per category and runs through `paweli test`. It is run four ways
# below, and each uses a different code path: interpreted, a .paw
# with embedded source, a .paw with stripped source, and a --bin
# executable. The "[OK] <name>" and "[FAIL] <name>" lines of each
# run are counted, so a category missing from main.pw is caught by
# count. No run has a terminal attached, so these lines are plain.

expected_count=$(grep -c '^@test ' "${SUITE}")

count_and_report() {
	local mode="$1"
	local output="$2"
	local code="$3"
	local ran

	ran=$(printf '%s\n' "${output}" | grep -cE '^\[(OK|FAIL)\] ')

	if [ "${code}" -eq 0 ] && [ "${ran}" -eq "${expected_count}" ]; then
		echo "PASS suite (${mode}): ${ran}/${expected_count} tests ran"
		pass=$((pass + 1))
	else
		echo "FAIL suite (${mode}): ${ran}/${expected_count} tests ran, exit code ${code}"
		printf '%s\n' "${output}" | sed 's/^/    /'
		fail=$((fail + 1))
	fi
}

# 1. Interpreted.
output=$(run_exe "${PAWELI}" test "${SUITE}")
code=$?
count_and_report "interpreted" "${output}" "${code}"

paw_tmp_dir=$(mktemp -d)
trap 'rm -rf "${paw_tmp_dir}"' EXIT

# 2 and 3. A compiled .paw, with embedded source and with
# --strip-source. Test mode is baked in at build time.
for variant in embedded stripped; do
	paw_file="${paw_tmp_dir}/suite.${variant}.paw"
	if [ "${variant}" = "stripped" ]; then
		strip_flag="--strip-source"
	else
		strip_flag=""
	fi

	build_output=$(run_exe "${PAWELI}" build "${SUITE}" "${paw_file}" --run-tests ${strip_flag})
	build_code=$?

	if [ "${build_code}" -ne 0 ]; then
		echo "FAIL suite (build --${variant})"
		echo "${build_output}" | sed 's/^/    /'
		fail=$((fail + 1))
		continue
	fi

	output=$(run_exe "${PAWELI}" "${paw_file}")
	code=$?
	count_and_report "${variant} .paw" "${output}" "${code}"
done

# 4. `paweli build --bin --run-tests` makes a standalone executable
# with test mode baked in. It runs with no `paweli` involved.
bin_file="${paw_tmp_dir}/suite.bin"
build_output=$(run_exe "${PAWELI}" build "${SUITE}" --bin "${bin_file}" --run-tests)
build_code=$?

if [ "${build_code}" -ne 0 ]; then
	echo "FAIL suite (build --bin)"
	echo "${build_output}" | sed 's/^/    /'
	fail=$((fail + 1))
else
	output=$(run_exe "${bin_file}${EXE_SUFFIX}")
	code=$?
	count_and_report "--bin executable" "${output}" "${code}"
fi

# 5. Project mode, end to end in a scratch directory. `paweli init`
# writes the manifest and main.pw. A second init refuses to
# overwrite them. `paweli run` and `paweli test` run the entry file
# with no build output. `paweli build` writes a .paw and a --bin
# under build/, and both run.
project_dir="${paw_tmp_dir}/project_smoke"
mkdir -p "${project_dir}"

init_output=$(cd "${project_dir}" && run_exe "${PAWELI}" init)
init_code=$?

if [ "${init_code}" -ne 0 ] || [ ! -f "${project_dir}/project.pawproj" ] || [ ! -f "${project_dir}/main.pw" ]; then
	echo "FAIL paweli init: manifest/main.pw not created (exit ${init_code})"
	echo "${init_output}" | sed 's/^/    /'
	fail=$((fail + 1))
else
	echo "PASS paweli init: wrote project.pawproj and main.pw"
	pass=$((pass + 1))
fi

reinit_output=$(cd "${project_dir}" && run_exe "${PAWELI}" init)
reinit_code=$?

if [ "${reinit_code}" -eq 0 ]; then
	echo "FAIL paweli init (already initialized): expected a nonzero exit, got 0"
	fail=$((fail + 1))
else
	echo "PASS paweli init (already initialized): refused to overwrite"
	pass=$((pass + 1))
fi

# `paweli init <name>` creates a directory first, which the init
# above does not test.
named_init_output=$(cd "${paw_tmp_dir}" && run_exe "${PAWELI}" init named_smoke)
named_init_code=$?

if [ "${named_init_code}" -ne 0 ] || [ ! -f "${paw_tmp_dir}/named_smoke/project.pawproj" ] || [ ! -f "${paw_tmp_dir}/named_smoke/main.pw" ]; then
	echo "FAIL paweli init <name>: directory/manifest/main.pw not created (exit ${named_init_code})"
	echo "${named_init_output}" | sed 's/^/    /'
	fail=$((fail + 1))
else
	echo "PASS paweli init <name>: created named_smoke/ with project.pawproj and main.pw"
	pass=$((pass + 1))
fi

run_output=$(cd "${project_dir}" && run_exe "${PAWELI}" run)
run_code=$?

if [ "${run_code}" -eq 0 ] && [ "${run_output}" = "Hello World!" ]; then
	echo "PASS paweli run (project mode)"
	pass=$((pass + 1))
else
	echo "FAIL paweli run (project mode): exit ${run_code}, output '${run_output}'"
	fail=$((fail + 1))
fi

test_output=$(cd "${project_dir}" && run_exe "${PAWELI}" test)
test_code=$?

# The starter main.pw has no @test functions, so `paweli test` must
# exit 0. This checks that project mode reaches the test path.
if [ "${test_code}" -eq 0 ]; then
	echo "PASS paweli test (project mode)"
	pass=$((pass + 1))
else
	echo "FAIL paweli test (project mode): exit ${test_code}, output '${test_output}'"
	fail=$((fail + 1))
fi

paw_build_output=$(cd "${project_dir}" && run_exe "${PAWELI}" build)
paw_build_code=$?

if [ "${paw_build_code}" -ne 0 ] || [ ! -f "${project_dir}/build/project_smoke.paw" ]; then
	echo "FAIL paweli build (project mode, .paw): exit ${paw_build_code}"
	echo "${paw_build_output}" | sed 's/^/    /'
	fail=$((fail + 1))
else
	run_output=$(run_exe "${PAWELI}" "${project_dir}/build/project_smoke.paw")
	if [ "${run_output}" = "Hello World!" ]; then
		echo "PASS paweli build (project mode, .paw)"
		pass=$((pass + 1))
	else
		echo "FAIL paweli build (project mode, .paw): unexpected output '${run_output}'"
		fail=$((fail + 1))
	fi
fi

bin_build_output=$(cd "${project_dir}" && run_exe "${PAWELI}" build --bin smokebin)
bin_build_code=$?

if [ "${bin_build_code}" -ne 0 ] || [ ! -x "${project_dir}/build/smokebin${EXE_SUFFIX}" ]; then
	echo "FAIL paweli build (project mode, --bin): exit ${bin_build_code}"
	echo "${bin_build_output}" | sed 's/^/    /'
	fail=$((fail + 1))
else
	run_output=$(run_exe "${project_dir}/build/smokebin${EXE_SUFFIX}")
	if [ "${run_output}" = "Hello World!" ]; then
		echo "PASS paweli build (project mode, --bin)"
		pass=$((pass + 1))
	else
		echo "FAIL paweli build (project mode, --bin): unexpected output '${run_output}'"
		fail=$((fail + 1))
	fi
fi

# 6. The corpus and highlight tests of the tree-sitter grammar. They
# do not use the paweli binary. PAWELI_SKIP_GRAMMAR_TESTS skips them.
TS_DIR="${REPO_ROOT}/packages/tree-sitter-paweli"
TS_BIN="${TS_DIR}/node_modules/.bin/tree-sitter"

if [ -n "${PAWELI_SKIP_GRAMMAR_TESTS:-}" ]; then
	echo "SKIP tree-sitter grammar tests"
elif [ ! -x "${TS_BIN}" ]; then
	echo "FAIL tree-sitter grammar tests: tree-sitter-cli not installed (run 'npm install' in ${TS_DIR})"
	fail=$((fail + 1))
else
	ts_output=$(cd "${TS_DIR}" && "${TS_BIN}" test 2>&1)
	ts_code=$?
	if [ "${ts_code}" -eq 0 ]; then
		echo "PASS tree-sitter grammar tests"
		pass=$((pass + 1))
	else
		echo "FAIL tree-sitter grammar tests"
		printf '%s\n' "${ts_output}" | sed 's/^/    /'
		fail=$((fail + 1))
	fi
fi

echo "──────────────────────────────"
echo "${pass} passed, ${fail} failed"

if [ "${fail}" -gt 0 ]; then
	exit 1
fi
