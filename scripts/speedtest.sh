#!/usr/bin/env bash
set -uo pipefail

# Compares run time across paweli's three run forms (.pw source, .paw
# bytecode, --bin executable) and a hand-written C version of the same
# program, for every benchmark under bench/pw/.

# Each C file (bench/c/<name>.c) uses the same algorithm and the same
# allocation strategy as its .pw version. array_sort grows its buffer
# from 8 by doubling, as object_array_push() does. string_build
# allocates a new buffer for every concatenation, as stdlib.concat
# does. This measures the cost of paweli's runtime for one algorithm.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
PAWELI="${REPO_ROOT}/build/release/paweli"
BENCH_DIR="${REPO_ROOT}/bench"
OUT_DIR="${REPO_ROOT}/build/bench"
CC="${CC:-cc}"

if [ ! -x "${PAWELI}" ]; then
	echo "release paweli binary not found at ${PAWELI}. Run 'make release' first" >&2
	exit 1
fi

if ! command -v hyperfine > /dev/null 2>&1; then
	echo "hyperfine not found on PATH. Install it to run this speedtest" >&2
	exit 1
fi

mkdir -p "${OUT_DIR}"

for pw_file in "${BENCH_DIR}"/pw/*.pw; do
	name=$(basename "${pw_file}" .pw)
	c_file="${BENCH_DIR}/c/${name}.c"

	if [ ! -f "${c_file}" ]; then
		echo "skipping ${name}: no matching bench/c/${name}.c" >&2
		continue
	fi

	echo "── ${name} ──────────────────────────────"

	paw_file="${OUT_DIR}/${name}.paw"
	bin_file="${OUT_DIR}/${name}_bin"
	c_bin="${OUT_DIR}/${name}_c"

	"${PAWELI}" build "${pw_file}" "${paw_file}" --strip-source || exit 1
	"${PAWELI}" build "${pw_file}" --bin "${bin_file}" --strip-source || exit 1
	"${CC}" -O3 -DNDEBUG -o "${c_bin}" "${c_file}" || exit 1

	hyperfine \
		--warmup 3 \
		--export-markdown "${OUT_DIR}/${name}.md" \
		-n "pw (source)" "${PAWELI} ${pw_file}" \
		-n "paw (bytecode)" "${PAWELI} ${paw_file}" \
		-n "bin (native exe)" "${bin_file}" \
		-n "C equivalent" "${c_bin}"

	echo
done

echo "──────────────────────────────"
echo "per-benchmark markdown reports written to ${OUT_DIR}/*.md"
