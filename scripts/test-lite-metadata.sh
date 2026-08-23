#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
sqlci=${LITE_SQLCI:-$repo_root/core/sql/lib/linux/64bit/debug/sqlci}
sql_lib_dir=${LITE_SQL_LIB_DIR:-$repo_root/core/sql/lib/linux/64bit/debug}
sqf_lib_dir=${LITE_SQF_LIB_DIR:-$repo_root/core/sqf/export/lib64d}
traf_home=$repo_root/core/sqf

[[ -x "$sqlci" ]] || {
  echo "FAIL: missing lite sqlci: $sqlci" >&2
  exit 2
}

store_dir=$(mktemp -d /tmp/traf-lite-metadata.XXXXXX)
trap 'rm -rf "$store_dir"' EXIT

output=$(
  env TRAF_HOME="$traf_home" \
    TRAF_LITE=1 \
    TRAF_LITE_STORE_DIR="$store_dir" \
    LD_LIBRARY_PATH="$sql_lib_dir:$sqf_lib_dir:${LD_LIBRARY_PATH:-}" \
    "$sqlci" <<'SQL'
CREATE TABLE LITE_MD_TEST (id INT NOT NULL, name VARCHAR(12), PRIMARY KEY(id));
CREATE INDEX LITE_MD_TEST_I ON LITE_MD_TEST(name);
SELECT CATALOG_NAME, SCHEMA_NAME, OBJECT_NAME, OBJECT_TYPE, OBJECT_UID
  FROM TRAFODION."_MD_".OBJECTS WHERE OBJECT_NAME = 'LITE_MD_TEST';
SELECT OBJECT_NAME FROM TRAFODION."_MD_".OBJECTS
  WHERE OBJECT_NAME = 'LITE_MD_TEST_I';
SELECT TABLE_UID, ROW_FORMAT, IS_AUDITED, FLAGS
  FROM TRAFODION."_MD_".TABLES;
SELECT OBJECT_UID, COLUMN_NAME, COLUMN_NUMBER, SQL_DATA_TYPE
  FROM TRAFODION."_MD_".COLUMNS;
SELECT OBJECT_UID, COLUMN_NAME, KEYSEQ_NUMBER, COLUMN_NUMBER
  FROM TRAFODION."_MD_".KEYS;
SELECT BASE_TABLE_UID, KEYTAG, IS_UNIQUE, INDEX_UID
  FROM TRAFODION."_MD_".INDEXES;
DROP TABLE LITE_MD_TEST;
exit;
SQL
)

if grep -q '\*\*\* ERROR' <<<"$output"; then
  echo "$output" >&2
  echo "FAIL: lite metadata SQL returned an error" >&2
  exit 1
fi
grep -q 'LITE_MD_TEST' <<<"$output" || {
  echo "$output" >&2
  echo "FAIL: OBJECTS did not expose the created table" >&2
  exit 1
}
grep -q 'LITE_MD_TEST_I' <<<"$output" || {
  echo "$output" >&2
  echo "FAIL: OBJECTS did not expose the secondary index" >&2
  exit 1
}
[[ $(grep -c -- '--- 1 row(s) selected.' <<<"$output") -eq 5 ]] || {
  echo "$output" >&2
  echo "FAIL: expected one metadata row in OBJECTS/TABLES/KEYS/INDEXES" >&2
  exit 1
}
grep -q -- '--- 2 row(s) selected.' <<<"$output" || {
  echo "$output" >&2
  echo "FAIL: COLUMNS did not expose both user columns" >&2
  exit 1
}

echo "lite metadata SQL check passed"
