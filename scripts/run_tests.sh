#!/usr/bin/env bash
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
PAWELI="${REPO_ROOT}/build/debug/paweli"

if [ ! -x "${PAWELI}" ]; then
	echo "paweli binary not found at ${PAWELI}. Run 'make build' first" >&2
	exit 1
fi

pass=0
fail=0

for pw_file in "${REPO_ROOT}"/tests/*.pw; do
	name=$(basename "${pw_file}")
	output=$("${PAWELI}" "${pw_file}" < /dev/null 2>&1)
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

# Every fixture is also round-tripped through `paweli build`: compiled to
# a .paw (once with source embedded, once with --strip-source), then run
# from the .paw directly, same pass/fail-by-exit-code convention as
# above. This exercises a genuinely different code path (paw.c's
# serializer/loader, no parser or compiler involved on the run side) that
# the plain loop above never touches at all.
paw_tmp_dir=$(mktemp -d)
trap 'rm -rf "${paw_tmp_dir}"' EXIT

for pw_file in "${REPO_ROOT}"/tests/*.pw; do
	name=$(basename "${pw_file}" .pw)

	for variant in embedded stripped; do
		paw_file="${paw_tmp_dir}/${name}.${variant}.paw"
		if [ "${variant}" = "stripped" ]; then
			strip_flag="--strip-source"
		else
			strip_flag=""
		fi

		build_output=$("${PAWELI}" build "${pw_file}" "${paw_file}" ${strip_flag} < /dev/null 2>&1)
		build_code=$?

		if [ "${build_code}" -ne 0 ]; then
			echo "FAIL ${name}.pw (build --${variant})"
			echo "${build_output}" | sed 's/^/    /'
			fail=$((fail + 1))
			continue
		fi

		run_output=$("${PAWELI}" "${paw_file}" < /dev/null 2>&1)
		run_code=$?

		if [ "${run_code}" -eq 0 ]; then
			echo "PASS ${name}.pw (${variant} .paw)"
			pass=$((pass + 1))
		else
			echo "FAIL ${name}.pw (${variant} .paw)"
			echo "${run_output}" | sed 's/^/    /'
			fail=$((fail + 1))
		fi
	done
done

# Same idea, one level further: `paweli build <file.pw> --bin <name>`
# produces a standalone native executable (a copy of this build's own
# paweli binary with the compiled program appended, see
# src/compiler/paw.c's paw_build_executable()). Run directly, with no
# `paweli` invocation anywhere in the run step. This is the actual
# point of the feature, so the test has to invoke the built binary on
# its own, not through `${PAWELI}`.
for pw_file in "${REPO_ROOT}"/tests/*.pw; do
	name=$(basename "${pw_file}" .pw)
	bin_file="${paw_tmp_dir}/${name}.bin"

	build_output=$("${PAWELI}" build "${pw_file}" --bin "${bin_file}" < /dev/null 2>&1)
	build_code=$?

	if [ "${build_code}" -ne 0 ]; then
		echo "FAIL ${name}.pw (build --bin)"
		echo "${build_output}" | sed 's/^/    /'
		fail=$((fail + 1))
		continue
	fi

	run_output=$("${bin_file}" < /dev/null 2>&1)
	run_code=$?

	if [ "${run_code}" -eq 0 ]; then
		echo "PASS ${name}.pw (--bin executable)"
		pass=$((pass + 1))
	else
		echo "FAIL ${name}.pw (--bin executable)"
		echo "${run_output}" | sed 's/^/    /'
		fail=$((fail + 1))
	fi
done

echo "──────────────────────────────"
echo "${pass} passed, ${fail} failed"

if [ "${fail}" -gt 0 ]; then
	exit 1
fi
