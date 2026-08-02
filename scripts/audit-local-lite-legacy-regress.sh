#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
regress_root="$repo_root/core/sql/regress"
manifest=${LOCAL_LITE_LEGACY_MANIFEST:-$regress_root/localLiteLegacy/manifest.tsv}
suites=(charsets compGeneral core executor fullstack2 privs1 privs2 seabase udr)
mode=check

usage() {
  cat <<'USAGE'
Usage: audit-local-lite-legacy-regress.sh [--check|--report|--candidates]

Validate and summarize the versioned local-lite legacy regress manifest.

  --check       verify inventory completeness and manifest vocabulary (default)
  --report      print disposition and milestone totals after validation
  --candidates  print static feature/safety hints; manual review is still required
USAGE
}

case "${1:-}" in
  ""|--check) mode=check ;;
  --report) mode=report ;;
  --candidates) mode=candidates ;;
  -h|--help) usage; exit 0 ;;
  *) usage >&2; exit 2 ;;
esac
[[ $# -le 1 ]] || { usage >&2; exit 2; }

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

[[ -f "$manifest" ]] || fail "missing manifest: $manifest"

work_dir=$(mktemp -d "${TMPDIR:-/tmp}/traf-local-lite-legacy-audit.XXXXXX")
trap 'rm -rf "$work_dir"' EXIT
inventory="$work_dir/inventory.tsv"
manifest_tests="$work_dir/manifest-tests.tsv"

has_expected() {
  local suite_dir=$1
  local test_name=$2
  local suffix=${test_name#TEST}
  compgen -G "$suite_dir/EXPECTED$suffix*" >/dev/null
}

for suite in "${suites[@]}"; do
  suite_dir="$regress_root/$suite"
  [[ -d "$suite_dir" ]] || fail "missing suite directory: $suite_dir"
  while IFS= read -r test_path; do
    test_name=$(basename "$test_path")
    [[ "$test_name" =~ ^TEST([0-9]+|RTS|TOK|TOK2)$ ]] || continue
    has_expected "$suite_dir" "$test_name" || continue
    printf '%s\t%s\n' "$suite" "$test_name"
  done < <(find "$suite_dir" -maxdepth 1 -type f -name 'TEST*' | sort)
done | sort -u >"$inventory"

valid_classifications='portable|mixed|hbase-physical|hive|service-stack|authorization|udr|compiler-tool|needs-review'
valid_dispositions='runnable|blocked|excluded|unsafe|needs-review'

line_number=0
while IFS=$'\t' read -r suite test_name selector classification milestone disposition blocker evidence extra; do
  line_number=$((line_number + 1))
  [[ -n "$suite" ]] || continue
  [[ "$suite" == \#* ]] && continue
  [[ "$suite" == "suite" && "$test_name" == "test" ]] && continue
  [[ -z "${extra:-}" ]] || fail "manifest line $line_number has too many columns"
  [[ -n "$test_name" && -n "$selector" && -n "$classification" &&
     -n "$milestone" && -n "$disposition" && -n "$blocker" &&
     -n "$evidence" ]] || fail "manifest line $line_number has an empty required column"
  [[ "$classification" =~ ^($valid_classifications)$ ]] ||
    fail "manifest line $line_number has invalid classification: $classification"
  [[ "$disposition" =~ ^($valid_dispositions)$ ]] ||
    fail "manifest line $line_number has invalid disposition: $disposition"
  [[ "$milestone" =~ ^M([0-9]|10)$|^-$ ]] ||
    fail "manifest line $line_number has invalid milestone: $milestone"
  [[ "$selector" == "all" || "$selector" =~ ^[A-Za-z0-9_,-]+$ ]] ||
    fail "manifest line $line_number has invalid selector: $selector"
  evidence_path=${evidence%%:*}
  [[ -e "$repo_root/$evidence_path" ]] ||
    fail "manifest line $line_number has missing evidence path: $evidence_path"
  printf '%s\t%s\n' "$suite" "$test_name" >>"$manifest_tests"
done <"$manifest"

sort -u -o "$manifest_tests" "$manifest_tests"

if ! diff -u "$inventory" "$manifest_tests" >"$work_dir/inventory.diff"; then
  cat "$work_dir/inventory.diff" >&2
  fail "manifest does not exactly cover the primary legacy TEST inventory"
fi

unsafe_reason() {
  local test_path=$1
  if rg -n -i '^[[:space:]]*(sh|shell)[[:space:]]|TESTABORT' "$test_path" \
      >/dev/null; then
    printf 'shell-or-abort'
  elif rg -n -i '^[[:space:]]*cleanup[[:space:]]+obsolete[[:space:]]+volatile[[:space:]]+tables' \
      "$test_path" >/dev/null; then
    printf 'crashing-volatile-cleanup'
  elif rg -n -i '^[[:space:]]*obey[[:space:]]+[^;]*\$\$[A-Za-z_][A-Za-z0-9_]*\$\$' \
    "$test_path" >/dev/null; then
    printf 'external-obey'
  else
    printf 'none'
  fi
}

while IFS=$'\t' read -r suite test_name; do
  test_path="$regress_root/$suite/$test_name"
  reason=$(unsafe_reason "$test_path")
  [[ "$reason" == "none" ]] && continue
  awk -F '\t' -v suite="$suite" -v test_name="$test_name" '
    $1 == suite && $2 == test_name && ($6 == "unsafe" || $6 == "excluded") {
      found = 1
    }
    END { exit(found ? 0 : 1) }
  ' "$manifest" ||
    fail "$suite/$test_name contains $reason but is not marked unsafe or excluded"
done <"$inventory"

feature_flags() {
  local test_path=$1
  local flags=()
  rg -i -q '\bupdate\b' "$test_path" && flags+=(update)
  rg -i -q '\bdelete[[:space:]]+from\b|\bmerge[[:space:]]+|\bupsert\b' \
    "$test_path" && flags+=(delete-merge-upsert)
  rg -i -q '\bcreate[[:space:]]+(unique[[:space:]]+)?index\b|\bdrop[[:space:]]+index\b' \
    "$test_path" && flags+=(index)
  rg -i -q '\b(create|drop)[[:space:]]+(view|schema|sequence|synonym)\b|\balter[[:space:]]+table\b|\btruncate[[:space:]]+table\b|\bforeign[[:space:]]+key\b|\breferences\b|\bcheck[[:space:]]*\(' \
    "$test_path" && flags+=(catalog-ddl)
  rg -i -q 'update[[:space:]]+statistics|showstats|showddl|trafodion\."_MD_"' \
    "$test_path" && flags+=(metadata-stats)
  rg -i -q 'ucs2|utf8|sjis|gb18030|kanji|ksc5601|collate|translate|\binterval\b|\bboolean\b|varbinary|\bbinary\b|\bclob\b|\bblob\b' \
    "$test_path" && flags+=(charset-type)
  rg -i -q '\bgrant\b|\brevoke\b|\bcreate[[:space:]]+role\b|register[[:space:]]+user' \
    "$test_path" && flags+=(authorization)
  rg -i -q '\bcreate[[:space:]]+(library|procedure|function)\b|\bcall[[:space:]]+' \
    "$test_path" && flags+=(udr)
  rg -i -q 'hbase|hadoop|hbase_options|salt[[:space:]]+using|hbase\."_(CELL|ROW)_"' \
    "$test_path" && flags+=(hbase-hadoop)
  if [[ ${#flags[@]} -eq 0 ]]; then
    printf 'none'
  else
    local joined
    joined=$(IFS=,; echo "${flags[*]}")
    printf '%s' "$joined"
  fi
}

if [[ "$mode" == "candidates" ]]; then
  printf 'suite\ttest\tsafety\tfeature_flags\n'
  while IFS=$'\t' read -r suite test_name; do
    test_path="$regress_root/$suite/$test_name"
    printf '%s\t%s\t%s\t%s\n' \
      "$suite" "$test_name" "$(unsafe_reason "$test_path")" \
      "$(feature_flags "$test_path")"
  done <"$inventory"
  exit 0
fi

inventory_count=$(wc -l <"$inventory")
echo "local-lite legacy manifest check passed: $inventory_count primary TEST inputs"

if [[ "$mode" == "report" ]]; then
  echo
  echo "Disposition by suite"
  awk -F '\t' '
    $1 !~ /^#/ && $1 != "suite" { count[$1 FS $6]++ }
    END {
      for (key in count) {
        split(key, parts, FS)
        printf "%s\t%s\t%d\n", parts[1], parts[2], count[key]
      }
    }
  ' "$manifest" | sort
  echo
  echo "Entries by milestone"
  awk -F '\t' '
    $1 !~ /^#/ && $1 != "suite" { count[$5]++ }
    END { for (key in count) printf "%s\t%d\n", key, count[key] }
  ' "$manifest" | sort -V
fi
