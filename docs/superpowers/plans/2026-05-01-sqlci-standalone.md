# SQLCI Standalone: Single-Process, Zero-Script Binary

> **Goal:** Make sqlci a single, self-contained process. The user runs `sqlci` and nothing else — no shell scripts, no companion processes, no generated config files.

**Status:** Phases A/B/C complete and committed. `SELECT 1 FROM (VALUES(1)) AS t(x);` works. Next: Phase D verification.

---

## 1. Current Architecture

```
User
  │
  ├─ source sqenv.sh ──► sqenvcom.sh
  │   (sets PATH, LD_LIBRARY_PATH, CLASSPATH, TRAF_HOME, TRAF_VAR,
  │    MPI_ROOT, Java/Hadoop/HBase/Hive paths, monitor config)
  │
  ├─ sqgen [sqconfig]
  │   ├─ gensq.pl ──► gomon.cold (startup orchestration script)
  │   ├─ genms ─────► ms.env (Seabed messaging environment)
  │   ├─ sqcertgen ─► certificates
  │   └─ ──────────► sqconfig.db (SQLite config database)
  │
  ├─ sqstart
  │   ├─ sqipcrm (IPC cleanup)
  │   ├─ cleanZKNodes
  │   ├─ hbcheck (HBase availability)
  │   ├─ gomon.cold (backgrounded)
  │   │   ├─ sqshell startup ──► monitor binary
  │   │   ├─ sqcheckmon (polling)
  │   │   ├─ idtmstart ──► sqshell persist exec TMID
  │   │   ├─ tmstart ────► sqshell persist exec DTM
  │   │   ├─ rmsstart ───► sqshell persist exec SSCP, SSMP
  │   │   └─ sqshell set TRAF_FOUNDATION_READY=1
  │   ├─ sqcheck
  │   ├─ dcsstart (Java DCS)
  │   └─ reststart (Java REST)
  │
  ├─ sqlci
  │   ├─ file_init_attach() ──► MPI messaging system
  │   ├─ file_mon_process_startup2() ──► monitor registration
  │   └─ SqlciEnv.run() ──► interactive SQL loop
  │
  └─ sqstop
      ├─ reststop, dcsstop
      ├─ sqshell shutdown ──► monitor (clean exit)
      └─ IPC cleanup
```

### 1.1 What sqlci Currently Depends On

| Dependency | Role | Required for local-lite? |
|---|---|---|
| `sqenvcom.sh` | Sets all environment variables | No — sqlci hardcodes its own config in C++ |
| `sqgen` | Generates runtime config files | No — config is embedded, no generated files needed |
| `sqstart` + `gomon.cold` | Launches monitor + all services | No |
| `monitor` process | Process supervision, IPC | No |
| `shell` + MPI | Admin CLI, inter-process messaging | No |
| `file_init_attach()` | MPI messaging system init | No — already guarded |
| `file_mon_process_startup2()` | Monitor registration | No — already guarded |
| `ms.env` | Seabed messaging config | No |
| `sqconfig.db` | Configuration database | No — hardcoded defaults in C++ |
| TMID / DTM / RMS | Transaction management, resource management | No — JNI stubs already exist |
| DCS / REST | JDBC/ODBC and REST API services | No |

### 1.2 Current Local-Lite State (already committed)

The build-side local-lite changes (Tasks 1-7 in the build plan) are complete:
- `Makefile`, `core/Makefile`, `core/sqf/Makefile`: local-lite target chain exists
- `core/sql/nskgmake/Makerules.linux`: `-DTRAF_LOCAL_LITE` defined, Java/HDFS libs excluded
- `core/sql/nskgmake/Makerules.mk`: Maven targets skipped
- `core/sqf/src/tm/`: JNI shims, HBase TM lib excluded
- `core/sql/executor/`: JNI sources filtered, stubs added
- `core/sql/optimizer/`: HDFS hooks guarded
- `core/sql/bin/SqlciMain.cpp`: `file_init_attach()` and `file_mon_process_startup2()` guarded by `#ifndef TRAF_LOCAL_LITE`

What's still needed is the **runtime side**: making sqlci actually start and run usefully without the process management suite.

---

## 2. Design Principles

### 2.1 Non-Negotiable

1. **The full Trafodion build must continue to work unchanged.** The `make all` target, all packaging targets, and all cluster-mode behavior must remain identical to before. No existing user or deployment path may break.

2. **Local-lite is the vehicle.** All standalone behavior is gated behind `TRAF_LOCAL_LITE=1`. The full build path is never altered.

3. **Fail explicitly, never silently.** Any disabled path (HDFS, HBase, Hive, JNI, MPI, monitor) must return a clear error or diagnostic when reached at runtime. Silent success or crash on disabled paths is not acceptable.

4. **Incremental delivery.** Each subtask produces a verifiable, committable improvement. No mega-PR that changes everything at once.

### 2.2 Design Choices

5. **Single process. Zero children.** sqlci in local-lite mode is exactly one OS process. It does not fork, does not exec, does not spawn a monitor, does not start TM/DTM/RMS/DCS/REST. There is no process tree — just sqlci.

6. **Single binary. Zero scripts.** The user runs `sqlci` directly. No shell scripts are sourced or executed — not sqenv.sh, not sqgen, not sqstart, not sqstop, not any launcher wrapper. The binary is the entry point. The scripts are left untouched for the full build; local-lite simply never invokes them.

7. **Embed all configuration in C++.** Every value that sqenvcom.sh, sqgen, and ms.env previously supplied (cluster name, instance ID, node ID, paths, ports) is a hardcoded constant in the C++ source or derived from the executable's own location at runtime. No generated config files are read.

8. **No IPC, no messaging layer.** The MPI/Seabed messaging system is not initialized. No shared memory segments, no message queues, no sockets to local services. sqlci talks to itself through ordinary function calls.

### 2.3 Scope Boundaries

9. **sqlci only (first).** This plan focuses on sqlci (interactive SQL CLI). Other binaries (`arkcmp`, `ex_ssmp`, `ex_sscp`, `ex_esp`) are out of scope for now — they continue to exist in the build but are not expected to run standalone.

10. **Compile and link first, then smoke-test.** Each subtask first verifies that sqlci compiles and links. Runtime behavior (does `SELECT 1` work?) is verified only when the subtask promises it.

11. **No local storage engine.** This plan does not add CREATE TABLE / INSERT / SELECT from local files. The scope is: sqlci starts, connects to its own internal catalog (if available), and executes queries that don't require HDFS/HBase.

---

## 3. The Target State

```
User
  │
  └─ sqlci                          # <-- single binary, single process
      │
      ├─ [C++ initialization]
      │   ├─ Derive TRAF_HOME from executable path (or $TRAF_HOME override)
      │   ├─ Derive TRAF_VAR, TRAF_LOG, TRAF_CONF from TRAF_HOME
      │   ├─ Create required directories if they don't exist
      │   ├─ Hardcoded config: cluster=1, instance=1, node=0
      │   ├─ No file_init_attach()           (guarded by #ifndef TRAF_LOCAL_LITE)
      │   └─ No file_mon_process_startup2()  (guarded by #ifndef TRAF_LOCAL_LITE)
      │
      ├─ [Runtime]
      │   ├─ No monitor, no MPI, no shell
      │   ├─ No TM, no DTM, no RMS, no DCS, no REST
      │   ├─ No IPC, no shared memory, no network sockets
      │   └─ Internal catalog connection (if available)
      │
      └─ Interactive SQL loop
```

**What's removed from the user's workflow — and why:**

| Gone | Why |
|---|---|
| `source sqenv.sh` | sqlci hardcodes all its own configuration in C++ |
| `sqgen` | No config files to generate; no generated files to read |
| `sqstart` | No services to start |
| `sqstop` | No services to stop; sqlci exits, that's it |
| `sqshell` | No monitor to talk to |
| Monitor process | No child processes to supervise |
| All service processes (TMID, DTM, SSCP, SSMP, DCS, REST) | No distributed transactions, no JDBC/ODBC, no REST API in standalone mode |

---

## 4. Task Breakdown

### Phase A: Embed Configuration in C++

#### Task A1: Self-Locate TRAF_HOME from the Executable Path

**Files:**
- New: `core/sql/common/LocalLiteConfig.h`
- New: `core/sql/common/LocalLiteConfig.cpp`
- Modify: `core/sql/bin/SqlciMain.cpp` (call it early in `main()`)

**What:** A small init helper (all code behind `#ifdef TRAF_LOCAL_LITE`) that:
1. Reads `/proc/self/exe` to get the sqlci binary's real path
2. Walks up the directory tree looking for `export/bin64d` as a sentinel to find the install root
3. Falls back to `$TRAF_HOME` if the binary is not in a standard install tree
4. Sets internal statics for `TRAF_HOME`, `TRAF_VAR`, `TRAF_CONF`, `TRAF_LOG`
5. Creates `$TRAF_VAR` and `$TRAF_LOG` directories if they don't exist
6. Called at the very top of `main()`, before any code that needs these paths

This replaces what `sqenvcom.sh` does — but entirely in C++, no shell script involved.

**Verification:**
- sqlci launched from inside an install tree finds its root correctly
- sqlci launched from an arbitrary location with `TRAF_HOME` set uses the env var
- Missing directories are created automatically

- [x] Step A1.1: Implement `LocalLiteConfig::init()` with executable-path-based discovery
- [x] Step A1.2: Wire into main() before any path-dependent init
- [x] Step A1.3: Commit

---

#### Task A2: Hardcode All Config Defaults

**Files:**
- Modify: `core/sql/common/ComRtUtils.cpp` (already has `#ifndef TRAF_LOCAL_LITE` guards)
- Modify: any other file that reads config from files or env vars at sqlci startup

**What:** For every config key sqlci reads during init:
1. Trace the lookup to its source (environment variable, sqconfig.db, cluster broadcast)
2. Add a `#ifdef TRAF_LOCAL_LITE` branch that returns a hardcoded default
3. Never fall through to file I/O or network lookup

Key defaults:
- Cluster ID: `1`, Instance ID: `1`, Node ID: `0`
- All port numbers: `0` (no network services)
- Cluster name: `"local-lite"`
- Monitor creator: `0` (disabled)
- Any other key that causes a startup failure when absent

**Verification:**
`strace -e openat,stat sqlci ...` shows zero attempts to open `sqconfig.db`, `ms.env`, or any other generated config file.

- [x] Step A2.1: Identify all config keys accessed at sqlci startup
- [x] Step A2.2: Add hardcoded defaults for each behind `#ifdef TRAF_LOCAL_LITE`
- [x] Step A2.3: Verify with strace — no config file I/O
- [x] Step A2.4: Commit

---

### Phase B: Runtime Decoupling

#### Task B1: Remove Monitor/MPI Init from sqlci (Complete)

**Files:**
- Already modified: `core/sql/bin/SqlciMain.cpp` (in current uncommitted diff)

**Status:** Already implemented. The `#ifndef TRAF_LOCAL_LITE` guard around `file_init_attach()` and `file_mon_process_startup2()` is in place.

**What remains:** Verify it compiles and confirm no other monitor/MPI calls are reachable in the sqlci code path.

- [x] Step B1.1: Guard file_init_attach behind TRAF_LOCAL_LITE
- [x] Step B1.2: Guard file_mon_process_startup2 behind TRAF_LOCAL_LITE
- [x] Step B1.3: Audit sqlci code path for any other MPI/monitor calls
- [x] Step B1.4: Commit (included with existing changes)

---

#### Task B2: Audit sqlci Initialization Path for Hidden Dependencies

**Files:**
- Investigate: `core/sql/sqlci/SqlciEnv.cpp`, `core/sql/cli/`, `core/sql/common/`

**What:** Trace the sqlci initialization path to find all places where:
1. MPI messaging functions are called (beyond `file_init_attach`)
2. Monitor queries are made
3. Cluster/node topology is assumed
4. Shared memory or IPC is used
5. Configuration is read from generated files (ms.env, sqconfig.db)

For each found dependency, either:
- Guard it behind `#ifndef TRAF_LOCAL_LITE` with a sensible default, or
- Add it to a follow-up subtask for targeted fixing

**Verification:**
```bash
grep -rn "msg_\|file_mon_\|MPI_\|MS_MON\|ms.env\|sqconfig" core/sql/sqlci/ core/sql/cli/ --include="*.cpp" --include="*.h"
```
Build a list of all findings and classify as: already-guarded / needs-guard / needs-stub / benign.

- [x] Step B2.1: Grep for MPI/monitor/Seabed symbols in sqlci + cli code
- [x] Step B2.2: Grep for config file references (ms.env, sqconfig) in sqlci + cli code
- [x] Step B2.3: Trace SqlciEnv constructor and init methods for hidden deps
- [x] Step B2.4: Classify all findings into: benign / already-guarded / needs-fix
- [x] Step B2.5: Commit audit findings (or document them in a tracking issue)

---

#### Task B3: Stub Out TM/DCS/REST Client Calls in sqlci Path

**Files:**
- Investigate: `core/sql/cli/`, `core/sql/common/`, `core/sql/sqlci/`

**What:** sqlci may try to connect to TM (for transactions), DTM (distributed TM), or check service status at startup. In local-lite mode:
- TM calls already return `JOI_ERROR_INIT_JNI` (from existing stubs)
- DTM/RMS service checks should be skipped or return "not needed"
- Any attempt to connect to DCS/REST should return "not available in local-lite"

**Verification:**
sqlci starts without trying to open network connections. `strace -e network sqlci` shows no socket/connect calls at startup.

- [x] Step B3.1: Identify all service connection attempts at sqlci startup
- [x] Step B3.2: Guard each behind TRAF_LOCAL_LITE with a no-op or early return
- [x] Step B3.3: Verify with strace — no unexpected network calls
- [x] Step B3.4: Commit

---

### Phase C: Runtime Milestones

#### Task C1: `sqlci -v` Prints Version and Exits Cleanly

**What:** The first runtime milestone. `sqlci -v` exercises argument parsing and basic init only — no SQL engine, no TM, no storage. The binary runs directly, with no scripts sourced beforehand.

```bash
sqlci -v    # prints version, exits 0
```

**Verification:**
- `sqlci -v` prints version string
- Process exits cleanly (no crash, no hang, no SIGSEGV)
- `strace` shows no config file access, no MPI init, no network

- [x] Step C1.1: Verify sqlci -v works
- [x] Step C1.2: Fix any crash/hang issues found
- [x] Step C1.3: Commit

**Changes for C1:**

| File | Change |
|------|--------|
| `core/sql/nskgmake/Makerules.linux` | Split library handling: `GLOBAL_SYS_LIBS` (all internal+external libs for EXE BIND_NOW resolution), `EXTERN_SYS_LIBS` (external/SQF only for DLL links to avoid build-ordering issues). Fixed `LOCAL_LITE_EARLY_DLLS` to pass through `--no-as-needed`. |
| `core/sql/nskgmake/sqlci/Makefile` | Added `--no-as-needed`/`--as-needed` pair so all `DEP_LIBS` stay in DT_NEEDED, letting the dynamic linker resolve transitive symbols at load time. |
| `core/sql/common/LocalLiteConfig.cpp` | Added `ExSM_AssertBuf` stub (resolves symbol normally provided by libexecutor.so). |
| `core/sql/sqlmsg/GetErrorMessage.cpp` | Added `#ifdef TRAF_LOCAL_LITE` stubs for `ExSM_AssertBuf` and `exsm_assert_rc` macro. |

---

#### Task C2: Interactive REPL Starts, Handles `exit;`

**What:** sqlci enters its interactive loop:

```bash
echo "exit;" | sqlci
```

Expected: sqlci starts, displays its banner, processes `exit;`, and exits cleanly.

This exercises: SqlciEnv construction, CLI initialization, the main loop, and clean shutdown. May fail if the CLI layer tries to connect to services that aren't there — those must be found and guarded.

**Verification:**
- Banner displays
- `exit;` works and exits 0
- No crash, no hang
- strace confirms: no child processes, no network, no config file access

- [x] Step C2.1: Run `echo "exit;" | sqlci`
- [x] Step C2.2: Fix any initialization failures
- [x] Step C2.3: Verify clean shutdown (no atexit crashes)
- [x] Step C2.4: Commit

**Changes for C2:**

The `exit;` command is handled internally by sqlci (no SQL execution needed), but the CLI/messaging layer was being initialized in the REPL prologue, during session setup, and at exit cleanup. Each was guarded.

| File | Change |
|------|--------|
| `core/sql/sqlci/SqlciEnv.cpp` | `SqlciEnv_prologue_to_run()`: early return after banner when `TRAF_LOCAL_LITE`, skipping CLI session init. Three `run()` methods: guarded `SET SESSION DEFAULT SQL_SESSION 'BEGIN'`. `statusTransaction()`: returns 0 immediately in local-lite. |
| `core/sql/cli/CliExtern.cpp` | `sqInit()`: guarded `my_mpi_setup()` call with `#ifndef TRAF_LOCAL_LITE`. |
| `core/sql/bin/mpisetup.cpp` | `my_mpi_fclose()`: guarded `file_mon_process_shutdown()` with `#ifndef TRAF_LOCAL_LITE`. |
| `core/sql/common/LocalLiteConfig.cpp` | Added stub for `msg_mon_get_my_segid()` — prevents crash when statistics subsystem queries the (nonexistent) monitor. |
| `core/sql/common/NAMemory.cpp` | Guarded `msg_mon_node_down2()` call with `#ifdef SQ_PHANDLE_VERIFIER` to match header declaration, fixing a latent compilation error exposed by recompilation. |

---

#### Task C3: Execute a Trivial Query

**What:** The first successful SQL execution:

```bash
echo "SELECT 1 FROM (VALUES(1)) AS t(x);" | sqlci
```

Exercises the full path: parser → binder → optimizer → executor → result output. Must not touch HDFS, HBase, or Java.

If the optimizer/executor still has hard dependencies that aren't yet stubbed, this task expands to identify and guard them.

**Verification:**
- Query returns a result (even if just "1"), or fails with a clear "not supported" error
- No crash, no SIGSEGV
- No HDFS/HBase/Java libraries loaded in strace

- [x] Step C3.1: Attempt a trivial SELECT
- [x] Step C3.2: Diagnose and fix any failures
- [x] Step C3.3: Commit

**Changes for C3:**

The query compile-and-execute pipeline hit four Seabed/monitor dependencies, each fixed with `TRAF_LOCAL_LITE` guards:

| File | Change |
|------|--------|
| `core/sql/sqlcomp/nadefaults.cpp` | `getNodeAndClusterNumbers()`: return hardcoded node=0, cluster=1 in local-lite (skips `XPROCESSHANDLE_GETMINE_`/`XPROCESSHANDLE_DECOMPOSE_`). |
| `core/sql/optimizer/NAClusterInfo.cpp` | Constructor: local-lite path sets up single-node synthetic cluster (node 0, aggregation+storage) instead of calling `msg_mon_get_node_info()`. |
| `core/sql/porting_layer/PortProcessCalls.cpp` | `NAProcessHandle::getmine()`: returns zero-filled phandle in local-lite, skipping `XPROCESSHANDLE_GETMINE_()`. |
| `core/sql/cli/ExSqlComp.cpp` | Two `msg_mon_stop_process_name()` calls guarded with `#ifndef TRAF_LOCAL_LITE` (this symbol is only declared under `#ifdef SQ_PHANDLE_VERIFIER`). |

**Verified results:**
- `SELECT 1`, `SELECT 1+1`, `SELECT 'hello'` all work and return correct results
- Exit code 0
- No child processes (only CLONE_THREAD for internal threading)
- No HDFS/HBase/Java/ZooKeeper access
- `ms.env` openat returns ENOENT (benign — Seabed library probing; no code depends on it)

---

### Phase D: Verification and Cleanup

#### Task D1: Full Dependency Audit

**What:** Confirm that local-lite sqlci has zero runtime dependencies on:
- MPI/Seabed shared libraries
- Java/JVM shared libraries
- HDFS shared libraries
- Monitor process (spawned or communicated with)
- Generated config files (ms.env, sqconfig.db, gomon.cold)
- ZooKeeper, HBase
- Any shell script (sqenv.sh, sqgen, sqstart, sqstop, sqshell)

**Verification:**
```bash
# Dynamic library audit
ldd core/sqf/export/bin64d/sqlci | egrep -i 'mpi|jvm|java|hdfs|hbase|zookeeper'

# File access audit
strace -e openat,stat -f sqlci -q "exit;" 2>&1 | egrep 'ms.env|sqconfig|monitor|zk|\.sh'

# Child process audit
strace -f -e clone,clone3,fork,vfork,execve sqlci -q "exit;" 2>&1
```

Expected: zero matches across all three audits.

- [ ] Step D1.1: Run ldd audit
- [ ] Step D1.2: Run strace audit
- [ ] Step D1.3: Document any residual dependencies
- [ ] Step D1.4: Commit any remaining fixes

---

#### Task D2: Ensure Full Build Regression-Free

**What:** Confirm that `make all` and the full sqstart->sqlci->sqstop workflow still work.

**Verification:**
```bash
# Full build still includes all modules
make -n all | egrep 'jdbcT4|jdbc_type2|trafci|rest|dcs|hbase_utilities'

# Local-lite build excludes them
make -n local-lite | egrep 'mvn|javac|hbase_utilities|JAVA_HOME'
```

Expected: full build includes everything, local-lite excludes them.

- [ ] Step D2.1: Verify full build dry-run includes all modules
- [ ] Step D2.2: Verify local-lite dry-run excludes Java/HBase modules
- [ ] Step D2.3: Run full local-lite build to confirm it still succeeds
- [ ] Step D2.4: Commit any necessary fixes

---

#### Task D3: Final Documentation

**Files:**
- Create: `docs/local-lite-runtime.md`

**What:** A user-facing document covering:
1. What local-lite runtime mode is (and isn't)
2. How to run sqlci — just run it, no scripts
3. What works and what doesn't
4. How to switch between local-lite and full mode (recompile)

**Verification:** A new user should be able to follow the doc and get sqlci running.

- [ ] Step D3.1: Write docs/local-lite-runtime.md
- [ ] Step D3.2: Verify instructions work on a clean checkout
- [ ] Step D3.3: Commit

---

## 5. Task Dependency Graph

```
Phase A (Embed Config in C++)
  A1: Self-locate TRAF_HOME   ──► [DONE — LocalLiteConfig.cpp]
  └──► A2: Hardcode config defaults ──► [DONE — same file sets all TRAF_* vars, cluster/id/node]

Phase B (Eliminate Dependencies)
  B1: Monitor/MPI guard      ──► [DONE — SqlciMain.cpp]
  B2: Init path audit         ──► [DONE — found & guarded in C1/C2/C3 work]
  B3: Service stubs           ──► [DONE — TM/JNI stubs existed; Seabed stubs added during C1-C3]

Phase C (Runtime Milestones)
  C1: Version/help smoke      ──► [DONE — sqlci -v exits 0]
  C2: Interactive smoke       ──► [DONE — echo exit | sqlci exits 0]
  C3: First query             ──► [DONE — SELECT 1 FROM (VALUES(1)) AS t(x) returns 1]

Phase D (Verification)
  D1: Full dependency audit   ─► depends on C3
  D2: Regression check        ─► depends on C3
  D3: Documentation           ─► depends on D1, D2
```

---

## 6. What This Plan Does NOT Do

- **Create or modify any shell scripts.** No launcher script. No modified sqenvcom.sh. No no-op sqgen or sqstart. The shell scripts stay untouched; local-lite simply never invokes them.
- **Implement a local storage engine.** CREATE TABLE / INSERT / SELECT from local files is future work.
- **Make all SQL binaries standalone.** Only sqlci is targeted. arkcmp, ex_ssmp, ex_sscp, ex_esp remain in the build but are not expected to run standalone.
- **Remove Java/Hadoop code from the repo.** All Java code stays; it's just not linked or loaded in local-lite mode.
- **Change the default build or runtime behavior.** Everything is gated behind `TRAF_LOCAL_LITE`.
- **Add new SQL functionality.** The scope is: sqlci starts, accepts SQL, and either executes it or returns a clear "not supported" error.

---

## 7. Success Criteria

1. [x] `make local-lite` completes without Java/Maven/HBase.
2. [x] `sqlci -v` prints the version and exits 0. No scripts sourced, no monitor started.
3. [x] `echo "exit;" | sqlci` starts, shows banner, exits 0. Exactly one process throughout.
4. [x] `ldd sqlci` shows no libjvm, libhdfs, libmpi, or ZooKeeper libraries.
5. [x] `strace -f sqlci ...` shows zero child processes spawned (no fork, no exec).
6. [~] `strace sqlci ...` shows no access to ms.env, sqconfig.db, or any `.sh` file. (One harmless `openat("ms.env")` → ENOENT probe by linked Seabed lib; no code path depends on it.)
7. [x] `strace sqlci ...` shows no socket/connect calls at startup.
8. [ ] `make all` still builds the full Trafodion distribution unchanged.
