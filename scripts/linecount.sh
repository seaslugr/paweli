#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

total=0

while IFS= read -r -d '' file; do
  lines=$(wc -l < "${file}")
  total=$((total + lines))
  printf "%6d  %s\n" "${lines}" "${file#"${REPO_ROOT}/"}"
done < <(find "${REPO_ROOT}/src" \( -name "*.c" -o -name "*.h" \) -print0 | sort -z)

echo "──────────────────────────────"
printf "%6d  total\n" "${total}"

echo
echo "book/"

book_total=0

while IFS= read -r -d '' file; do
  lines=$(wc -l < "${file}")
  book_total=$((book_total + lines))
  printf "%6d  %s\n" "${lines}" "${file#"${REPO_ROOT}/"}"
done < <(find "${REPO_ROOT}/book/src" -name "*.md" -print0 | sort -z)

echo "──────────────────────────────"
printf "%6d  total\n" "${book_total}"
