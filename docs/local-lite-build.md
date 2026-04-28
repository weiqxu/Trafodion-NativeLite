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

## Native Dependencies

Install the native package set needed by local-lite:

```bash
scripts/install-local-lite-deps.sh --dry-run
scripts/install-local-lite-deps.sh -y
```

The installer sets up C/C++ build tools, MPICH, Thrift, log4cxx, protobuf,
curl, OpenSSL, readline, ncurses, bison, flex, Perl, Python, and related native
development headers. It intentionally does not install Java, Maven, Hadoop,
HBase, or Hive.

On distributions where MPICH headers are not laid out like Trafodion's legacy
tools tree, the script creates a repository-local compatibility directory at
`core/sqf/opt/local-lite-mpich`. The `local-lite` target uses that directory
automatically when present.

## Current Status

The local-lite graph has been trimmed to skip Maven, Java client modules,
HBase utilities, HBase transaction jar setup, `hbasetmlib2`, JVM link flags,
and global HDFS/JVM SQL link flags.

In this workspace, `make local-lite` reaches SQF setup without invoking the full
Java/Hadoop build checks. The local-lite target supplies the minimum SQF path
defaults normally provided by `sqenv.sh`, including `TRAF_HOME=core/sqf`.

The current blockers are native third-party build dependencies, such as MPICH
and Thrift development libraries. In this workspace, the build currently stops
on missing native Thrift shared libraries. Those blockers are native build
environment setup, not remaining Java, Maven, HDFS, or HBase dependencies in
the local-lite graph.

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
