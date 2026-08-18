#!/usr/bin/env bash
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
PAWELI="${REPO_ROOT}/build/debug/paweli"
SUITE="${REPO_ROOT}/tests/suite/main.pw"

if [ ! -x "${PAWELI}" ]; then
	echo "paweli binary not found at ${PAWELI}. Run 'make build' first" >&2
	exit 1
fi

pass=0
fail=0

# The whole suite is one mega program (tests/suite/main.pw importing
# category modules via the module system), driven through @test/
# `paweli test`. It's run four different ways below, each exercising a
# genuinely different code path: direct interpretation, a compiled .paw
# (embedded and stripped source), and a standalone --bin executable.
# Every mode's own "[OK] <name>"/"[FAIL] <name>" lines are captured so a
# category silently missing from main.pw's @test wrappers (never wired
# up, or dropped by a bad edit) is caught by count, not just by the exit
# code. None of these invocations run with a tty attached, so
# $test_report_pass()/$test_report_failure() (src/stdlib/stdlib.c)
# always print the plain, uncolored form of these lines here.

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

# 1. Direct interpretation.
output=$("${PAWELI}" test "${SUITE}" < /dev/null 2>&1)
code=$?
count_and_report "interpreted" "${output}" "${code}"

paw_tmp_dir=$(mktemp -d)
trap 'rm -rf "${paw_tmp_dir}"' EXIT

# 2 & 3. Compiled .paw, embedded source and --strip-source, run mode
# baked in at build time (no --run-tests needed/allowed when running
# the .paw itself).
for variant in embedded stripped; do
	paw_file="${paw_tmp_dir}/suite.${variant}.paw"
	if [ "${variant}" = "stripped" ]; then
		strip_flag="--strip-source"
	else
		strip_flag=""
	fi

	build_output=$("${PAWELI}" build "${SUITE}" "${paw_file}" --run-tests ${strip_flag} < /dev/null 2>&1)
	build_code=$?

	if [ "${build_code}" -ne 0 ]; then
		echo "FAIL suite (build --${variant})"
		echo "${build_output}" | sed 's/^/    /'
		fail=$((fail + 1))
		continue
	fi

	output=$("${PAWELI}" "${paw_file}" < /dev/null 2>&1)
	code=$?
	count_and_report "${variant} .paw" "${output}" "${code}"
done

# 4. `paweli build <file.pw> --bin <name> --run-tests` produces a
# standalone native executable with the run mode baked in. Run
# directly, with no `paweli` invocation anywhere in the run step.
bin_file="${paw_tmp_dir}/suite.bin"
build_output=$("${PAWELI}" build "${SUITE}" --bin "${bin_file}" --run-tests < /dev/null 2>&1)
build_code=$?

if [ "${build_code}" -ne 0 ]; then
	echo "FAIL suite (build --bin)"
	echo "${build_output}" | sed 's/^/    /'
	fail=$((fail + 1))
else
	output=$("${bin_file}" < /dev/null 2>&1)
	code=$?
	count_and_report "--bin executable" "${output}" "${code}"
fi

# 5. `paweli init`, `paweli run`/`paweli test` (project.pawproj, no
# filename argument), and project-mode `paweli build`, driven end to
# end in a scratch directory: the manifest and starter main.pw
# actually get written, a second init refuses to clobber them, run/
# test execute the entry file directly with no build artifact
# involved, and build's output forms (.paw and --bin) come out under
# build/ and actually run.
project_dir="${paw_tmp_dir}/project_smoke"
mkdir -p "${project_dir}"

init_output=$(cd "${project_dir}" && "${PAWELI}" init < /dev/null 2>&1)
init_code=$?

if [ "${init_code}" -ne 0 ] || [ ! -f "${project_dir}/project.pawproj" ] || [ ! -f "${project_dir}/main.pw" ]; then
	echo "FAIL paweli init: manifest/main.pw not created (exit ${init_code})"
	echo "${init_output}" | sed 's/^/    /'
	fail=$((fail + 1))
else
	echo "PASS paweli init: wrote project.pawproj and main.pw"
	pass=$((pass + 1))
fi

reinit_output=$(cd "${project_dir}" && "${PAWELI}" init < /dev/null 2>&1)
reinit_code=$?

if [ "${reinit_code}" -eq 0 ]; then
	echo "FAIL paweli init (already initialized): expected a nonzero exit, got 0"
	fail=$((fail + 1))
else
	echo "PASS paweli init (already initialized): refused to overwrite"
	pass=$((pass + 1))
fi

# init'd a fresh named subdirectory too, since `paweli init <name>` has
# its own directory-creation path init'ing "." above never exercises.
named_init_output=$(cd "${paw_tmp_dir}" && "${PAWELI}" init named_smoke < /dev/null 2>&1)
named_init_code=$?

if [ "${named_init_code}" -ne 0 ] || [ ! -f "${paw_tmp_dir}/named_smoke/project.pawproj" ] || [ ! -f "${paw_tmp_dir}/named_smoke/main.pw" ]; then
	echo "FAIL paweli init <name>: directory/manifest/main.pw not created (exit ${named_init_code})"
	echo "${named_init_output}" | sed 's/^/    /'
	fail=$((fail + 1))
else
	echo "PASS paweli init <name>: created named_smoke/ with project.pawproj and main.pw"
	pass=$((pass + 1))
fi

run_output=$(cd "${project_dir}" && "${PAWELI}" run < /dev/null 2>&1)
run_code=$?

if [ "${run_code}" -eq 0 ] && [ "${run_output}" = "Hello World!" ]; then
	echo "PASS paweli run (project mode)"
	pass=$((pass + 1))
else
	echo "FAIL paweli run (project mode): exit ${run_code}, output '${run_output}'"
	fail=$((fail + 1))
fi

test_output=$(cd "${project_dir}" && "${PAWELI}" test < /dev/null 2>&1)
test_code=$?

# main.pw's starter program has no @test functions, so `paweli test`
# running it should still exit 0 (main() just isn't called); this is
# mainly confirming project-mode dispatch reaches run_source() with
# run_tests forced on, not exercising real test output.
if [ "${test_code}" -eq 0 ]; then
	echo "PASS paweli test (project mode)"
	pass=$((pass + 1))
else
	echo "FAIL paweli test (project mode): exit ${test_code}, output '${test_output}'"
	fail=$((fail + 1))
fi

paw_build_output=$(cd "${project_dir}" && "${PAWELI}" build < /dev/null 2>&1)
paw_build_code=$?

if [ "${paw_build_code}" -ne 0 ] || [ ! -f "${project_dir}/build/project_smoke.paw" ]; then
	echo "FAIL paweli build (project mode, .paw): exit ${paw_build_code}"
	echo "${paw_build_output}" | sed 's/^/    /'
	fail=$((fail + 1))
else
	run_output=$("${PAWELI}" "${project_dir}/build/project_smoke.paw" < /dev/null 2>&1)
	if [ "${run_output}" = "Hello World!" ]; then
		echo "PASS paweli build (project mode, .paw)"
		pass=$((pass + 1))
	else
		echo "FAIL paweli build (project mode, .paw): unexpected output '${run_output}'"
		fail=$((fail + 1))
	fi
fi

bin_build_output=$(cd "${project_dir}" && "${PAWELI}" build --bin smokebin < /dev/null 2>&1)
bin_build_code=$?

if [ "${bin_build_code}" -ne 0 ] || [ ! -x "${project_dir}/build/smokebin" ]; then
	echo "FAIL paweli build (project mode, --bin): exit ${bin_build_code}"
	echo "${bin_build_output}" | sed 's/^/    /'
	fail=$((fail + 1))
else
	run_output=$("${project_dir}/build/smokebin" < /dev/null 2>&1)
	if [ "${run_output}" = "Hello World!" ]; then
		echo "PASS paweli build (project mode, --bin)"
		pass=$((pass + 1))
	else
		echo "FAIL paweli build (project mode, --bin): unexpected output '${run_output}'"
		fail=$((fail + 1))
	fi
fi

# 6. The Helix tree-sitter grammar's own corpus (parse-tree) and
# highlight (capture) tests, independent of the paweli binary above.
TS_DIR="${REPO_ROOT}/packages/tree-sitter-paweli"
TS_BIN="${TS_DIR}/node_modules/.bin/tree-sitter"

if [ ! -x "${TS_BIN}" ]; then
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
