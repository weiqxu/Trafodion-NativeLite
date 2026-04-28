# Trafodion Local-Lite

This checkout is configured for the `local-lite` build target. `local-lite`
builds a native Trafodion core subset without Java, Maven, HDFS, HBase, Hive,
DCS, REST, TrafCI, or Java client modules.

The target is useful for native SQF/SQL build work and link validation. It is
not a complete standalone SQL database mode: there is no local storage engine,
and Hadoop-backed SQL paths are intentionally disabled.

The original Apache Trafodion project overview is preserved in
`README.trafodion.md`.

## Scope

`local-lite` builds the native core pieces needed for the trimmed SQF/SQL graph:

- SQF setup, monitor, nameserver, watchdog, process starter, and shell tools
- TM and native security components
- SQL compiler/runtime shared libraries
- SQL command-line binaries such as `sqlci` and `tdm_arkcmp`

It intentionally skips:

- Java, Maven, and Java client modules
- HDFS, HBase, Hive, DCS, REST, TrafCI, and ODB modules
- HBase transaction jars and JVM/HDFS link dependencies

## Dependencies

Install the native package set:

```bash
scripts/install-local-lite-deps.sh --dry-run
scripts/install-local-lite-deps.sh -y
```

The installer covers C/C++ build tools, MPICH, Thrift, log4cxx, protobuf,
SQLite, curl, OpenSSL, readline, ncurses, bison, flex, Perl, Python, and native
development headers. It deliberately does not install Java, Maven, Hadoop,
HBase, or Hive.

If you cannot install packages system-wide, make sure equivalent development
headers and unversioned linker names are available. For example, linking with
`-lsqlite3` requires a `libsqlite3.so` development-library name, not only the
runtime `libsqlite3.so.0`.

## Build

From the repository root:

```bash
OMPI_CXX=/usr/bin/g++ make local-lite
```

`OMPI_CXX=/usr/bin/g++` forces the OpenMPI C++ wrapper to use the system C++
compiler. This avoids failures when `PATH` contains another toolchain whose
startup objects do not match the host C runtime.

The top-level target forwards through `core/Makefile` and builds the native
subset with `TRAF_LOCAL_LITE=1`.

## Build Outputs

Main exported binaries are written to:

```text
core/sqf/export/bin64d/
```

Common entries include:

```text
monitor
trafns
sqwatchdog
pstartd
monmemlog
tm
trafconf
shell
sqlci
tdm_arkcmp
```

Exported shared libraries are written or linked through:

```text
core/sqf/export/lib64d/
```

The SQL debug output directory is:

```text
core/sql/lib/linux/64bit/debug/
```

## Runtime Environment

For direct use of the built native tools:

```bash
cd core/sqf

export TRAF_LOCAL_LITE=1
export TRAF_HOME=$PWD
export SQ_BUILD_TYPE=debug
export SQ_MTYPE=64
export SQ_MBTYPE=64d
export TRAF_VAR=$TRAF_HOME/tmp
export TRAF_LOG=$TRAF_HOME/logs
export TRAF_CONF=$TRAF_HOME/conf

export PATH=$TRAF_HOME/export/bin64d:$TRAF_HOME/sql/scripts:$TRAF_HOME/tools:$PATH
export LD_LIBRARY_PATH=$TRAF_HOME/export/lib64d:/usr/lib/x86_64-linux-gnu:/lib/x86_64-linux-gnu:${LD_LIBRARY_PATH:-}

mkdir -p "$TRAF_VAR" "$TRAF_LOG"
```

Then run native tools directly, for example:

```bash
sqvers
trafconf -help
sqshell -help
sqlci -help
```

## Starting SQF

The legacy SQF scripts are still present. A minimal single-node attempt follows
the normal Trafodion sequence:

```bash
cd core/sqf
source ./sqenv.sh
sqgen "$TRAF_CONF/sqconfig"
sqstart -c
sqcheck
```

Stop the environment with:

```bash
sqstop
```

This startup path still contains full Trafodion assumptions, including HBase and
ZooKeeper checks. A successful `local-lite` build does not guarantee that
`sqstart` can launch a usable SQL environment without additional runtime
trimming. Local-lite runtime work should fail closed on disabled HDFS, Hive, and
HBase paths instead of silently succeeding.

## Verification

Check that the local-lite dry run does not route through disabled Java/Hadoop
build modules:

```bash
make -n local-lite | egrep 'mvn|javac|hbase-trx|hbasetmlib2|hbase_utilities|JAVA_HOME'
```

Expected output: no matching lines.

After a successful local-lite link, check produced binaries and libraries for
JVM or HDFS dynamic dependencies:

```bash
find core/sqf/export -type f -perm -111 -print | xargs -r ldd | egrep 'libjvm|libhdfs'
```

Expected output: no matching lines.

The default Trafodion build remains available through the existing targets such
as `make all`.
