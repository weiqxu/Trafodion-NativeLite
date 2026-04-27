# Local-Lite Build

`local-lite` builds a native Trafodion core subset without Java, Maven, HDFS,
HBase, Hive, DCS, REST, TrafCI, or Java client modules.

This target is a build-slimming milestone. It does not implement a local storage
engine, and Hadoop-backed SQL paths are intentionally unsupported.

## Build

```bash
make local-lite
```

The target forwards through `core/Makefile` and builds the SQF/SQL native subset
with `TRAF_LOCAL_LITE=1`.

## Current Status

The local-lite graph has been trimmed to skip Maven, Java client modules,
HBase utilities, HBase transaction jar setup, `hbasetmlib2`, JVM link flags,
and global HDFS/JVM SQL link flags.

In this workspace, `make local-lite` reaches SQF setup without invoking the full
Java/Hadoop build checks, then stops because the native Trafodion build
environment is not initialized. The observed blocker is an empty `TRAF_HOME`,
which produces paths such as `/macros.gmk` and `WROOT=/../sql`.

That blocker is native build environment setup, not a remaining Java, Maven,
HDFS, or HBase dependency in the local-lite graph.

## Verification

Check that the local-lite dry-run does not route through disabled Java/Hadoop
build modules:

```bash
make -n local-lite | egrep 'mvn|javac|hbase-trx|hbasetmlib2|hbase_utilities|JAVA_HOME'
```

Expected output: no matching lines. If the native environment is not initialized,
`make` may still exit nonzero after reporting the native setup failure.

After a successful local-lite link, check produced binaries and libraries for
JVM or HDFS dynamic dependencies:

```bash
find core/sqf/export -type f -perm -111 -print | xargs -r ldd | egrep 'libjvm|libhdfs'
```

Expected output after a successful link: no matching lines.

## Disabled Runtime Behavior

HDFS, Hive, and HBase paths are intentionally disabled. If a local-lite binary
reaches one of those paths, it must return an unsupported-operation error
instead of silently succeeding.

The default Trafodion build remains unchanged. Use `make all` or the existing
component targets for the full Java/Hadoop-backed build.
