#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
regress_root="$repo_root/core/sql/regress"
legacy_dir="$regress_root/liteLegacy"
legacy_manifest="$legacy_dir/manifest.tsv"
extra_manifest="$legacy_dir/standard-extra-manifest.tsv"
newregr_manifest="$legacy_dir/newregr-inventory.tsv"
mode=check

usage() {
  cat <<'USAGE'
Usage: audit-lite-upstream-regress.sh [--check|--report|--list-newregr]

Audit the complete standard runallsb inventory and the separate newregr asset
inventory. This command inventories unassessed tests; it does not execute Hive,
QAT, or newregr workloads.

  --check          validate manifests and current source assets (default)
  --report         print standard and newregr disposition summaries
  --list-newregr  list every paired and unpaired newregr input
USAGE
}

case "${1:-}" in
  ""|--check) mode=check ;;
  --report) mode=report ;;
  --list-newregr) mode=list-newregr ;;
  -h|--help) usage; exit 0 ;;
  *) usage >&2; exit 2 ;;
esac
[[ $# -le 1 ]] || { usage >&2; exit 2; }

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

for required in "$legacy_manifest" "$extra_manifest" "$newregr_manifest"; do
  [[ -f "$required" ]] || fail "missing inventory manifest: $required"
done
"$repo_root/scripts/audit-lite-legacy-regress.sh" --check >/dev/null

work_dir=$(mktemp -d "${TMPDIR:-/tmp}/traf-lite-upstream-audit.XXXXXX")
trap 'rm -rf "$work_dir"' EXIT
extra_inventory="$work_dir/standard-extra-inventory.tsv"
extra_manifest_cases="$work_dir/standard-extra-manifest-cases.tsv"
newregr_pairs="$work_dir/newregr-pairs.tsv"
newregr_unpaired="$work_dir/newregr-unpaired.tsv"
dynamic_summary="$work_dir/newregr-summary.tsv"
manifest_summary="$work_dir/newregr-manifest-summary.tsv"
: >"$extra_inventory"
: >"$extra_manifest_cases"
: >"$newregr_pairs"
: >"$newregr_unpaired"

shopt -s nullglob

for test_path in "$regress_root"/hive/TEST[0-9]*; do
  test_name=$(basename "$test_path")
  [[ "$test_name" =~ ^TEST[0-9]+$ ]] || continue
  suffix=${test_name#TEST}
  compgen -G "$regress_root/hive/EXPECTED$suffix*" >/dev/null || continue
  printf 'hive\t%s\n' "$test_name" >>"$extra_inventory"
done

for test_path in "$regress_root"/qat/qatddl[0-9][0-9] \
                 "$regress_root"/qat/qatdml[0-9][0-9]; do
  test_name=$(basename "$test_path")
  [[ -f "$regress_root/qat/e$test_name" ]] ||
    fail "QAT input has no baseline: $test_name"
  printf 'qat\t%s\n' "$test_name" >>"$extra_inventory"
done
sort -u -o "$extra_inventory" "$extra_inventory"

line_number=0
while IFS=$'\t' read -r suite test_name classification disposition blocker evidence extra; do
  line_number=$((line_number + 1))
  [[ -n "$suite" ]] || continue
  [[ "$suite" == \#* ]] && continue
  [[ "$suite" == "suite" && "$test_name" == "test" ]] && continue
  [[ -z "${extra:-}" ]] ||
    fail "standard extra manifest line $line_number has too many columns"
  [[ -n "$test_name" && -n "$classification" && -n "$disposition" &&
     -n "$blocker" && -n "$evidence" ]] ||
    fail "standard extra manifest line $line_number has an empty required column"
  [[ "$suite" =~ ^(hive|qat)$ ]] ||
    fail "standard extra manifest line $line_number has invalid suite: $suite"
  [[ "$classification" =~ ^(hive|mixed|stateful)$ ]] ||
    fail "standard extra manifest line $line_number has invalid classification: $classification"
  [[ "$disposition" =~ ^(blocked|excluded)$ ]] ||
    fail "standard extra manifest line $line_number has invalid disposition: $disposition"
  [[ "$suite" != "hive" ||
     ( "$classification" == "hive" && "$disposition" == "excluded" ) ]] ||
    fail "Hive case $test_name must remain an explicit hive exclusion"
  [[ "$suite" != "qat" || "$disposition" == "blocked" ]] ||
    fail "QAT case $test_name must remain blocked until an executable adapter passes"
  [[ -e "$repo_root/$evidence" ]] ||
    fail "standard extra manifest line $line_number has missing evidence: $evidence"
  printf '%s\t%s\n' "$suite" "$test_name" >>"$extra_manifest_cases"
done <"$extra_manifest"
sort -u -o "$extra_manifest_cases" "$extra_manifest_cases"

if ! diff -u "$extra_inventory" "$extra_manifest_cases" >"$work_dir/extra.diff"; then
  cat "$work_dir/extra.diff" >&2
  fail "standard extra manifest does not exactly cover Hive and QAT"
fi

record_newregr() {
  local suite=$1
  local input=$2
  local baseline=$3
  if [[ -f "$baseline" ]]; then
    printf '%s\t%s\t%s\n' "$suite" \
      "${input#$repo_root/}" "${baseline#$repo_root/}" >>"$newregr_pairs"
  else
    printf '%s\t%s\tmissing-baseline\n' "$suite" \
      "${input#$repo_root/}" >>"$newregr_unpaired"
  fi
}

for input in "$regress_root"/newregr/card/TEST*; do
  record_newregr card "$input" "${input%/*}/E$(basename "$input")"
done
for suite in mvqr mvs triggers; do
  for input in "$regress_root"/newregr/"$suite"/TEST*; do
    suffix=$(basename "$input")
    suffix=${suffix#TEST}
    record_newregr "$suite" "$input" "${input%/*}/EXPECTED$suffix"
  done
done
for input in "$regress_root"/newregr/opt/optddl[0-9][0-9] \
             "$regress_root"/newregr/opt/optdml[0-9][0-9] \
             "$regress_root"/newregr/opt/optfst[0-9][0-9]; do
  record_newregr opt "$input" "${input%/*}/e$(basename "$input")"
done
for input in "$regress_root"/newregr/parallel/test[0-9][0-9][0-9]; do
  record_newregr parallel "$input" "${input%/*}/e$(basename "$input")"
done
for input in "$regress_root"/newregr/rowsets/teste*.sql; do
  record_newregr rowsets "$input" "${input%.sql}.exp"
done
sort -o "$newregr_pairs" "$newregr_pairs"
sort -o "$newregr_unpaired" "$newregr_unpaired"

for suite in card mvqr mvs opt parallel rowsets triggers; do
  paired=$(awk -F '\t' -v suite="$suite" '$1 == suite { count++ } END { print count + 0 }' \
    "$newregr_pairs")
  unpaired=$(awk -F '\t' -v suite="$suite" '$1 == suite { count++ } END { print count + 0 }' \
    "$newregr_unpaired")
  printf '%s\t%d\t%d\n' "$suite" "$paired" "$unpaired" >>"$dynamic_summary"
done
for suite in exeperf perf; do
  [[ -d "$regress_root/newregr/$suite" ]] || fail "missing newregr workload: $suite"
  printf '%s\t0\t0\n' "$suite" >>"$dynamic_summary"
done
sort -o "$dynamic_summary" "$dynamic_summary"

line_number=0
while IFS=$'\t' read -r suite paired unpaired kind disposition blocker evidence extra; do
  line_number=$((line_number + 1))
  [[ -n "$suite" ]] || continue
  [[ "$suite" == \#* ]] && continue
  [[ "$suite" == "suite" ]] && continue
  [[ -z "${extra:-}" ]] ||
    fail "newregr manifest line $line_number has too many columns"
  [[ "$paired" =~ ^[0-9]+$ && "$unpaired" =~ ^[0-9]+$ ]] ||
    fail "newregr manifest line $line_number has invalid counts"
  [[ "$disposition" == "unassessed" ]] ||
    fail "newregr suite $suite must not be reported as executed"
  [[ -n "$kind" && -n "$blocker" && -e "$repo_root/$evidence" ]] ||
    fail "newregr manifest line $line_number has incomplete evidence"
  printf '%s\t%s\t%s\n' "$suite" "$paired" "$unpaired" >>"$manifest_summary"
done <"$newregr_manifest"
sort -o "$manifest_summary" "$manifest_summary"

if ! diff -u "$dynamic_summary" "$manifest_summary" >"$work_dir/newregr.diff"; then
  cat "$work_dir/newregr.diff" >&2
  fail "newregr summary manifest does not match current paired inputs"
fi

legacy_count=$(awk -F '\t' '!/^#/ && $1 != "suite" { key[$1 FS $2] = 1 }
  END { print length(key) }' "$legacy_manifest")
hive_count=$(awk -F '\t' '$1 == "hive" { count++ } END { print count + 0 }' "$extra_manifest")
qat_count=$(awk -F '\t' '$1 == "qat" { count++ } END { print count + 0 }' "$extra_manifest")
standard_count=$((legacy_count + hive_count + qat_count))
newregr_paired=$(wc -l <"$newregr_pairs")
newregr_unpaired_count=$(wc -l <"$newregr_unpaired")

if [[ "$mode" == "list-newregr" ]]; then
  printf 'status\tsuite\tinput\tbaseline\n'
  awk -F '\t' 'BEGIN { OFS = FS } { print "paired", $1, $2, $3 }' "$newregr_pairs"
  awk -F '\t' 'BEGIN { OFS = FS } { print "unpaired", $1, $2, $3 }' "$newregr_unpaired"
  exit 0
fi

if [[ "$mode" == "report" ]]; then
  echo "Standard runallsb inventory (kept separate from newregr)"
  printf 'audited-legacy\t%d\n' "$legacy_count"
  awk -F '\t' '
    BEGIN { priority["runnable"]=5; priority["blocked"]=4; priority["needs-review"]=3; priority["unsafe"]=2; priority["excluded"]=1 }
    !/^#/ && $1 != "suite" {
      key=$1 FS $2
      if (!(key in best) || priority[$6] > priority[best[key]]) best[key]=$6
    }
    END { for (key in best) count[best[key]]++; for (state in count) printf "legacy-%s\t%d\n", state, count[state] }
  ' "$legacy_manifest" | sort
  printf 'hive-excluded\t%d\n' "$hive_count"
  printf 'qat-blocked-unassessed\t%d\n' "$qat_count"
  printf 'standard-total\t%d\n' "$standard_count"
  echo
  echo "Separate newregr asset inventory (no execution result claimed)"
  awk -F '\t' '!/^#/ && $1 != "suite" { printf "%s\tpaired=%s\tunpaired=%s\t%s\n", $1, $2, $3, $5 }' \
    "$newregr_manifest"
  printf 'newregr-paired-total\t%d\n' "$newregr_paired"
  printf 'newregr-unpaired-total\t%d\n' "$newregr_unpaired_count"
  if [[ -f "$regress_root/tools/runregr_other.ksh" ]]; then
    printf 'generic-newregr-runner\tpresent\n'
  else
    printf 'generic-newregr-runner\tmissing-runregr_other.ksh\n'
  fi
  exit 0
fi

printf 'lite upstream regress inventory check passed: standard=%d (legacy=%d hive=%d qat=%d), newregr-paired=%d, newregr-unpaired=%d\n' \
  "$standard_count" "$legacy_count" "$hive_count" "$qat_count" \
  "$newregr_paired" "$newregr_unpaired_count"
