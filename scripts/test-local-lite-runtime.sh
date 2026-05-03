#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
sqstart="$repo_root/core/sqf/sql/scripts/sqstart"
sqenvcom="$repo_root/core/sqf/sqenvcom.sh"

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

grep -q 'TRAF_LOCAL_LITE' "$sqstart" || fail "sqstart must gate Hadoop/HBase startup checks on TRAF_LOCAL_LITE"
grep -q 'Skipping ZooKeeper cleanup and HBase availability check for local-lite' "$sqstart" ||
  fail "sqstart must document the local-lite skip in startup output"

grep -q 'TRAF_LOCAL_LITE' "$sqenvcom" || fail "sqenvcom must gate Hadoop/HBase classpath setup on TRAF_LOCAL_LITE"
grep -q 'Skipping Hadoop/HBase classpath setup for local-lite' "$sqenvcom" ||
  fail "sqenvcom must document the local-lite classpath skip"
grep -q 'SQ_CLASSPATH=' "$sqenvcom" || fail "sqenvcom must clear SQ_CLASSPATH for local-lite"

grep -q 'hbcheck 4 30' "$sqstart" || fail "default sqstart path must still run hbcheck"
grep -q 'cleanZKNodes' "$sqstart" || fail "default sqstart path must still clean ZooKeeper nodes"

sq_classpath=$(
  cd "$repo_root/core/sqf"
  TRAF_LOCAL_LITE=1 TRAF_CLUSTER_ID=1 TRAF_INSTANCE_ID=1 bash -lc \
    'source ./sqenv.sh >/dev/null 2>/dev/null; printf "%s" "$SQ_CLASSPATH"'
)

[[ -z "$sq_classpath" ]] || fail "local-lite SQ_CLASSPATH must be empty, got: $sq_classpath"

echo "local-lite runtime script checks passed"
