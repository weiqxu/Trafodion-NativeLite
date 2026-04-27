# Local-Lite Build Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a `local-lite` build target that compiles and links a native Trafodion core subset without requiring Java, Maven, HDFS, HBase, Hive, REST, DCS, or Java clients.

**Architecture:** Keep the existing full build unchanged and add a parallel `local-lite` build graph. Pass `TRAF_LOCAL_LITE=1` through native make targets, remove Java/Hadoop global link/include flags in that mode, skip Java/HBase build modules, and add small native stubs only where the linker proves disabled paths are still referenced.

**Tech Stack:** GNU make, Trafodion nskgmake, C/C++, shell verification commands.

---

## File Structure

- Modify `Makefile`: add the top-level `local-lite` forwarding target.
- Modify `core/Makefile`: add native-only `local-lite` target and keep existing `all` unchanged.
- Modify `core/sqf/Makefile`: add local-lite component graph without `hbase_utilities`.
- Modify `core/sqf/sql/Makefile`: add local-lite SQL target that passes `TRAF_LOCAL_LITE=1`.
- Modify `core/sqf/src/tm/Makefile`: add local-lite branch that avoids `JAVA_HOME`, `-ljvm`, `hbase-trx`, and `hbasetmlib2`.
- Modify `core/sqf/src/tm/macros.gmk`: remove `-lshbasetmlib`, Java include paths, and HBase TM include path when `TRAF_LOCAL_LITE=1`.
- Modify `core/sqf/src/tm/tmlib.h`: provide local-lite JNI type shims and avoid including real `jni.h`.
- Modify `core/sqf/src/tm/tmlib.cpp`: make `initJNI()` and `setupJNI()` return local-lite unsupported behavior without touching JVM.
- Modify `core/sql/nskgmake/Makerules.linux`: remove global Java/HDFS include and link flags when `TRAF_LOCAL_LITE=1`.
- Modify `core/sql/nskgmake/Makerules.mk`: skip `mavenbuild`, `mavenbuild_hdp`, and `mavenbuild_apache` when `TRAF_LOCAL_LITE=1`.
- Modify `core/sql/nskgmake/arkcmp/Makefile` and `core/sql/nskgmake/sqlci/Makefile`: remove `-ljsig` in local-lite.
- Modify `core/sql/nskgmake/executor/Makefile`: exclude JNI-backed executor sources in local-lite and add local-lite stubs as needed.
- Modify `core/sql/optimizer/HDFSHook.h` and `core/sql/optimizer/HDFSHook.cpp`: guard `hdfs.h` and libhdfs-only code in local-lite.
- Create `core/sql/executor/LocalLiteStorageStubs.cpp`: native unsupported stubs for residual HDFS/Hive/HBase client references.
- Create `core/sql/optimizer/LocalLiteHdfsHookStubs.cpp`: native unsupported or empty HDFS stats stubs if optimizer link requires them.
- Create `docs/local-lite-build.md`: document the target, scope, and verification commands.

## Task 1: Add Top-Level Local-Lite Targets

**Files:**
- Modify: `Makefile`
- Modify: `core/Makefile`

- [ ] **Step 1: Add top-level forwarding target**

In `Makefile`, add this target after `all`:

```make
.PHONY: local-lite
local-lite:
	@echo "Building local-lite Trafodion native core"
	cd core && $(MAKE) local-lite
```

- [ ] **Step 2: Add core native-only target**

In `core/Makefile`, extend `.PHONY` to include `local-lite`, then add:

```make
local-lite: $(MPI_TARGET) dbsecurity local-lite-foundation $(SEAMONSTER_TARGET)

local-lite-foundation: sqroot dbsecurity $(MPI_TARGET) $(SEAMONSTER_TARGET)
	cd sqf && $(MAKE) local-lite TRAF_LOCAL_LITE=1
```

Do not add `jdbc_jar`, `jdbc_type2_jar`, `dcs`, `rest`, `trafci`, `odb`, `lib_mgmt`, or package targets to `local-lite`.

- [ ] **Step 3: Verify target routing does not invoke Java modules**

Run:

```bash
make -n local-lite
```

Expected: output includes `cd core && make local-lite` and does not include `conn/jdbcT4`, `conn/jdbc_type2`, `conn/trafci`, `../dcs`, or `rest`.

- [ ] **Step 4: Commit**

```bash
git add Makefile core/Makefile
git commit -m "build: add local-lite entry targets"
```

## Task 2: Add SQF Local-Lite Component Graph

**Files:**
- Modify: `core/sqf/Makefile`
- Modify: `core/sqf/sql/Makefile`

- [ ] **Step 1: Add local-lite component list**

In `core/sqf/Makefile`, keep existing `SQ_COMPONENTS` unchanged and add:

```make
LOCAL_LITE_COMPONENTS := make_sqevlog seabed stfs trafconf $(MISC) tm $(SEAMONSTER_TARGET) make_sql_local_lite make_monitor tools
```

- [ ] **Step 2: Add local-lite target**

Add:

```make
local-lite: genverhdr $(LOCAL_LITE_COMPONENTS)
```

- [ ] **Step 3: Add local-lite SQL rule without hbase_utilities**

Add this rule near `make_sql`:

```make
make_sql_local_lite: $(SEAMONSTER_TARGET) win
	cd sql; $(MAKE) local-lite WROOT=$(SQL_W) TRAF_LOCAL_LITE=1 2>&1 | sed -e "s/$$/	##(SQL-LOCAL-LITE)/" ; exit $${PIPESTATUS[0]}
	cd $(TRAF_HOME)/sql/scripts && ./makemsg.ksh 2>&1 | sed -e "s/$$/	##(SQL-LOCAL-LITE)/" ; exit $${PIPESTATUS[0]}
```

This deliberately omits `hbase_utilities`.

- [ ] **Step 4: Add SQF SQL local-lite target**

In `core/sqf/sql/Makefile`, add:

```make
local-lite:
	cd stublibs; $(MAKE)
	cd $(WROOT)/nskgmake; $(MAKE) TRAF_LOCAL_LITE=1 linux$(SQ_BUILD_TYPE)
```

- [ ] **Step 5: Verify hbase_utilities is not in dry-run graph**

Run:

```bash
cd core/sqf && make -n local-lite TRAF_LOCAL_LITE=1
```

Expected: output includes `make_sql_local_lite` and does not include `cd hbase_utilities`.

- [ ] **Step 6: Commit**

```bash
git add core/sqf/Makefile core/sqf/sql/Makefile
git commit -m "build: add SQF local-lite graph"
```

## Task 3: Remove SQL Global Java and HDFS Flags in Local-Lite

**Files:**
- Modify: `core/sql/nskgmake/Makerules.linux`
- Modify: `core/sql/nskgmake/Makerules.mk`
- Modify: `core/sql/nskgmake/arkcmp/Makefile`
- Modify: `core/sql/nskgmake/sqlci/Makefile`

- [ ] **Step 1: Add local-lite compiler define**

In `core/sql/nskgmake/Makerules.linux`, after `SYS_DEFS += -DSQ_CPP_INTF`, add:

```make
ifeq ($(TRAF_LOCAL_LITE),1)
SYS_DEFS += -DTRAF_LOCAL_LITE
endif
```

- [ ] **Step 2: Split Java/HDFS global libraries**

Replace:

```make
GLOBAL_SYS_LIBS := -L $(LOC_JVMLIBS) $(LIBHDFS_LIB) $(THRIFT_LIB) $(LIBCURL_LIB) $(LOG4CXX_LIB)
```

with:

```make
ifeq ($(TRAF_LOCAL_LITE),1)
GLOBAL_SYS_LIBS := $(THRIFT_LIB) $(LIBCURL_LIB) $(LOG4CXX_LIB)
else
GLOBAL_SYS_LIBS := -L $(LOC_JVMLIBS) $(LIBHDFS_LIB) $(THRIFT_LIB) $(LIBCURL_LIB) $(LOG4CXX_LIB)
endif
```

- [ ] **Step 3: Split Java include paths**

Replace the `SYS_INCLUDES` block with a local-lite-aware form:

```make
SYS_INCLUDES :=
SYS_INCLUDES += -I$(TRAF_HOME)/inc \
	-I$(TRAF_HOME)/export/include \
	-I$(PROTOBUFS_INC) \
	$(THRIFT_INC) $(LOG4CXX_INC) $(LIBCURL_INC)

ifneq ($(TRAF_LOCAL_LITE),1)
SYS_INCLUDES += -I$(JAVA_HOME)/include -I$(JAVA_HOME)/include/linux
endif
```

- [ ] **Step 4: Skip SQL Maven jar targets**

In `core/sql/nskgmake/Makerules.mk`, replace:

```make
buildall: $(FINAL_LIBS) $(FINAL_DLLS) $(FINAL_INSTALL_OBJS) $(FINAL_EXES) mavenbuild mavenbuild_hdp mavenbuild_apache 
```

with:

```make
ifeq ($(TRAF_LOCAL_LITE),1)
buildall: $(FINAL_LIBS) $(FINAL_DLLS) $(FINAL_INSTALL_OBJS) $(FINAL_EXES)
else
buildall: $(FINAL_LIBS) $(FINAL_DLLS) $(FINAL_INSTALL_OBJS) $(FINAL_EXES) mavenbuild mavenbuild_hdp mavenbuild_apache
endif
```

- [ ] **Step 5: Remove jsig early link in local-lite**

In both `core/sql/nskgmake/arkcmp/Makefile` and `core/sql/nskgmake/sqlci/Makefile`, replace:

```make
EARLY_DLLS:= -ljsig
```

with:

```make
ifeq ($(TRAF_LOCAL_LITE),1)
EARLY_DLLS :=
else
EARLY_DLLS := -ljsig
endif
```

- [ ] **Step 6: Verify dry-run excludes Maven and Java link libraries**

Run:

```bash
cd core/sql/nskgmake && make TRAF_LOCAL_LITE=1 -n linuxdebug
```

Expected: output does not include `mavenbuild`, `mvn`, `-ljvm`, `-lhdfs`, or `-ljsig`.

- [ ] **Step 7: Commit**

```bash
git add core/sql/nskgmake/Makerules.linux core/sql/nskgmake/Makerules.mk core/sql/nskgmake/arkcmp/Makefile core/sql/nskgmake/sqlci/Makefile
git commit -m "build: remove SQL Java and HDFS globals in local-lite"
```

## Task 4: Add TM Local-Lite Build Mode

**Files:**
- Modify: `core/sqf/src/tm/Makefile`
- Modify: `core/sqf/src/tm/macros.gmk`
- Modify: `core/sqf/src/tm/tmlib.h`
- Modify: `core/sqf/src/tm/tmlib.cpp`

- [ ] **Step 1: Make TM macros local-lite aware**

In `core/sqf/src/tm/macros.gmk`, replace:

```make
LIBSTM          =  $(LIBSTMB) -lsxatmlib -lshbasetmlib $(LIBS) $(DEBUG) 
```

with:

```make
ifeq ($(TRAF_LOCAL_LITE),1)
LIBSTM          =  $(LIBSTMB) -lsxatmlib $(LIBS) $(DEBUG)
else
LIBSTM          =  $(LIBSTMB) -lsxatmlib -lshbasetmlib $(LIBS) $(DEBUG)
endif
```

Replace the single `INCLUDES = ...` line with:

```make
INCLUDES	= -I$(INCEXPDIR) -I$(INCMONDIR) -I$(MY_SPROOT)/export/include -I$(MY_SPROOT)/source/publications -I$(PROTOBUFS)/include -I$(LOG4CXX_INC_DIR) -I$(LOG4CXX_INC_DIR)/log4cxx -I$(LIBCLOGGER) -I$(TRAF_HOME)/src/tm
ifneq ($(TRAF_LOCAL_LITE),1)
INCLUDES	+= -I$(HBASETMLIB) -I$(INC_JAVA) -I$(INC_JAVALINUX)
endif
```

- [ ] **Step 2: Guard Java linker flags in TM Makefile**

In `core/sqf/src/tm/Makefile`, replace:

```make
LIBJVM        = -I. -I$(JAVA_HOME)/include -I$(JAVA_HOME)/include/linux -L$(JAVA_HOME)/jre/lib/amd64/server -ljvm
```

with:

```make
ifeq ($(TRAF_LOCAL_LITE),1)
LIBJVM        =
else
LIBJVM        = -I. -I$(JAVA_HOME)/include -I$(JAVA_HOME)/include/linux -L$(JAVA_HOME)/jre/lib/amd64/server -ljvm
endif
```

- [ ] **Step 3: Exclude JavaObjectInterfaceTM object in local-lite**

After `LIBSTMOBJS = ...`, add:

```make
ifeq ($(TRAF_LOCAL_LITE),1)
LIBSTMOBJS := $(filter-out $(OUTDIR)/javaobjectinterfacetm.o,$(LIBSTMOBJS))
endif
```

- [ ] **Step 4: Make HBase transaction jar target a no-op in local-lite**

Replace `cp_trx_jar` with:

```make
ifeq ($(TRAF_LOCAL_LITE),1)
cp_trx_jar:
	@echo "Skipping hbase-trx jar for local-lite"
else
cp_trx_jar:
	cd $(HBASE_TRX_LOC); make all
endif
```

- [ ] **Step 5: Do not build libshbasetmlib in local-lite**

Guard the `$(LIBEXPDIR)/libshbasetmlib.so` rule:

```make
ifneq ($(TRAF_LOCAL_LITE),1)
$(LIBEXPDIR)/libshbasetmlib.so: $(LIBSXATMOBJS) cp_trx_jar
	cd $(HBASETMLIB); $(MAKE)
endif
```

Also change the `$(OUTDIR)/tm` prerequisites so `$(LIBEXPDIR)/libshbasetmlib.so` is present only outside local-lite:

```make
ifeq ($(TRAF_LOCAL_LITE),1)
TM_HBASE_PREREQ :=
else
TM_HBASE_PREREQ := $(LIBEXPDIR)/libshbasetmlib.so
endif

$(OUTDIR)/tm: $(TMOBJS) $(LIBEXPDIR)/libsxatmlib.so $(TM_HBASE_PREREQ) $(PROGS) $(LIBEXPDIR)/libxarm.so cp_trx_jar
```

- [ ] **Step 6: Add JNI shims in tmlib.h**

In `core/sqf/src/tm/tmlib.h`, replace:

```cpp
#include <jni.h>
```

with:

```cpp
#ifdef TRAF_LOCAL_LITE
typedef void JNIEnv;
typedef void *jclass;
typedef void *jmethodID;
struct JavaMethodInit {
    std::string jm_name;
    std::string jm_signature;
    jmethodID methodID;
};
class JavaObjectInterfaceTM
{
protected:
    JavaObjectInterfaceTM(int debugPort = 0, int debugTimeout = 0) {}
    virtual ~JavaObjectInterfaceTM() {}
};
#else
#include <jni.h>
#endif
```

Move `#include "javaobjectinterfacetm.h"` under `#ifndef TRAF_LOCAL_LITE`:

```cpp
#ifndef TRAF_LOCAL_LITE
#include "javaobjectinterfacetm.h"
#endif
```

- [ ] **Step 7: Make JNI methods fail closed in local-lite**

In `core/sqf/src/tm/tmlib.cpp`, guard `TMLIB::initJNI()` and `TMLIB::setupJNI()`:

```cpp
#ifdef TRAF_LOCAL_LITE
int TMLIB::initJNI()
{
   return JOI_ERROR_INIT_JNI;
}

short TMLIB::setupJNI()
{
   return JOI_ERROR_INIT_JNI;
}
#else
// existing implementations remain here
#endif
```

If `JOI_ERROR_INIT_JNI` is unavailable after the shim, define a local-lite constant near the shim:

```cpp
#ifdef TRAF_LOCAL_LITE
static const int JOI_ERROR_INIT_JNI = 8;
#endif
```

- [ ] **Step 8: Verify TM dry-run excludes hbase-trx and libjvm**

Run:

```bash
cd core/sqf/src/tm && make -n TRAF_LOCAL_LITE=1
```

Expected: output does not include `hbase-trx`, `hbasetmlib2`, `JAVA_HOME`, or `-ljvm`.

- [ ] **Step 9: Commit**

```bash
git add core/sqf/src/tm/Makefile core/sqf/src/tm/macros.gmk core/sqf/src/tm/tmlib.h core/sqf/src/tm/tmlib.cpp
git commit -m "build: add TM local-lite mode"
```

## Task 5: Trim SQL Executor JNI Sources

**Files:**
- Modify: `core/sql/nskgmake/executor/Makefile`
- Create: `core/sql/executor/LocalLiteStorageStubs.cpp`

- [ ] **Step 1: Filter JNI-heavy executor sources**

In `core/sql/nskgmake/executor/Makefile`, after the current `CPPSRC` assignment, add:

```make
ifeq ($(TRAF_LOCAL_LITE),1)
CPPSRC := $(filter-out \
	JavaObjectInterface.cpp \
	HdfsClient_JNI.cpp \
	HiveClient_JNI.cpp \
	HBaseClient_JNI.cpp \
	SequenceFileReader.cpp \
	OrcFileReader.cpp,$(CPPSRC))
CPPSRC += LocalLiteStorageStubs.cpp
DEFS += -DTRAF_LOCAL_LITE
endif
```

- [ ] **Step 2: Create initial storage stub source**

Create `core/sql/executor/LocalLiteStorageStubs.cpp`:

```cpp
// @@@ START COPYRIGHT @@@
// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements. See the NOTICE file
// distributed with this work for additional information.
// @@@ END COPYRIGHT @@@

#ifdef TRAF_LOCAL_LITE

#include "Platform.h"

extern "C" const char *trafLocalLiteUnsupportedStorage()
{
  return "HDFS, Hive, and HBase are not supported in local-lite builds";
}

#endif
```

This seed file gives the build a dedicated place for symbols discovered in the first local-lite link attempt.

- [ ] **Step 3: Run executor-only build to discover required residual symbols**

Run:

```bash
cd core/sql/nskgmake && make TRAF_LOCAL_LITE=1 linuxdebug executor.so
```

Expected: either executor links or the first failure is an undefined reference to a disabled HDFS/Hive/HBase/JNI symbol.

- [ ] **Step 4: Add only proven missing symbols to LocalLiteStorageStubs.cpp**

For each undefined reference from Step 3, add a stub with the same signature from the corresponding header and return the existing failure code from that API family. Use these defaults:

```cpp
// HDFS scan/client methods return HDFS_SCAN_ERROR_STOP_EXCEPTION or
// HDFS_CLIENT_ERROR_HDFS_OPEN_EXCEPTION when no narrower code exists.
// Hive client methods return HVC_ERROR_INIT_EXCEPTION when no narrower code exists.
// HBase client methods return HTC_ERROR_INIT_EXCEPTION when no narrower code exists.
```

Do not stub symbols that are not reported by the linker.

- [ ] **Step 5: Re-run executor build until it links or reaches a non-storage failure**

Run:

```bash
cd core/sql/nskgmake && make TRAF_LOCAL_LITE=1 linuxdebug executor.so
```

Expected: no references to `JNI_CreateJavaVM`, `HdfsClient_JNI`, `HiveClient_JNI`, `HBaseClient_JNI`, `SequenceFileReader`, or `OrcFileReader` remain unresolved.

- [ ] **Step 6: Commit**

```bash
git add core/sql/nskgmake/executor/Makefile core/sql/executor/LocalLiteStorageStubs.cpp
git commit -m "build: trim executor JNI sources in local-lite"
```

## Task 6: Guard Optimizer HDFS Hook

**Files:**
- Modify: `core/sql/optimizer/HDFSHook.h`
- Modify: `core/sql/optimizer/HDFSHook.cpp`
- Create if needed: `core/sql/optimizer/LocalLiteHdfsHookStubs.cpp`
- Modify if stub is created: `core/sql/nskgmake/optimizer/Makefile`

- [ ] **Step 1: Guard hdfs.h include**

In `core/sql/optimizer/HDFSHook.h`, replace:

```cpp
#include "hdfs.h"
```

with:

```cpp
#ifndef TRAF_LOCAL_LITE
#include "hdfs.h"
#else
typedef void *hdfsFS;
#endif
```

- [ ] **Step 2: Compile optimizer in local-lite**

Run:

```bash
cd core/sql/nskgmake && make TRAF_LOCAL_LITE=1 linuxdebug optimizer.so
```

Expected: optimizer either compiles or fails in HDFS hook implementation code that directly calls libhdfs APIs.

- [ ] **Step 3: Guard libhdfs implementation blocks**

In `core/sql/optimizer/HDFSHook.cpp`, wrap libhdfs-only method bodies with:

```cpp
#ifndef TRAF_LOCAL_LITE
// existing libhdfs implementation
#else
// local-lite implementation returns FALSE, empty stats, or an unavailable diagnostic
#endif
```

Use the existing method signatures exactly. For boolean validation and connection helpers, return `FALSE`. For void disconnect/cleanup helpers, do nothing. For population methods with diagnostics, mark the operation unsuccessful using the existing `HHDFSDiags` API visible in the method body.

- [ ] **Step 4: Add separate stubs only if HDFSHook.cpp becomes too tangled**

If guarding `HDFSHook.cpp` creates broad unrelated edits, create `core/sql/optimizer/LocalLiteHdfsHookStubs.cpp` and add it under `ifeq ($(TRAF_LOCAL_LITE),1)` in `core/sql/nskgmake/optimizer/Makefile`. Keep `HDFSHook.cpp` out of `CPPSRC` in that same branch.

- [ ] **Step 5: Re-run optimizer build**

Run:

```bash
cd core/sql/nskgmake && make TRAF_LOCAL_LITE=1 linuxdebug optimizer.so
```

Expected: optimizer links without needing `hdfs.h` or `libhdfs`.

- [ ] **Step 6: Commit**

```bash
git add core/sql/optimizer/HDFSHook.h core/sql/optimizer/HDFSHook.cpp core/sql/optimizer/LocalLiteHdfsHookStubs.cpp core/sql/nskgmake/optimizer/Makefile
git commit -m "build: guard optimizer HDFS hooks in local-lite"
```

If `LocalLiteHdfsHookStubs.cpp` is not created, omit it from `git add`.

## Task 7: Run First Full Local-Lite Build and Fix Proven Link Gaps

**Files:**
- Modify only files already touched by Tasks 1-6 unless the linker identifies a new exact dependency.

- [ ] **Step 1: Run local-lite build**

Run:

```bash
make local-lite
```

Expected: build proceeds past Java/Maven/HBase utility/TM jar stages. It may fail on native compile or link gaps.

- [ ] **Step 2: Classify the first failure**

Use this classification:

```text
Java tool invocation: fix build graph; no local-lite target may call mvn, javac, hbase-trx, hbasetmlib2, or require JAVA_HOME.
Missing Java/Hadoop header: add or extend TRAF_LOCAL_LITE guard around the include.
Undefined HDFS/Hive/HBase/JNI symbol: add a minimal unsupported stub in the appropriate local-lite stub file.
Unrelated native dependency: leave behavior unchanged and fix only the local-lite build path.
```

- [ ] **Step 3: Fix one failure class at a time**

After each fix, rerun:

```bash
make local-lite
```

Expected: the previous failure does not recur.

- [ ] **Step 4: Verify no Java/Hadoop dynamic dependencies in produced SQL binaries**

Run after successful link:

```bash
find core/sqf/export -type f -perm -111 -print | xargs -r ldd | egrep 'libjvm|libhdfs'
```

Expected: no output.

- [ ] **Step 5: Commit final link-gap fixes**

```bash
git add Makefile core/Makefile core/sqf core/sql docs/local-lite-build.md
git commit -m "build: complete local-lite native link"
```

Only include files actually changed by this task.

## Task 8: Document Verification

**Files:**
- Create: `docs/local-lite-build.md`

- [ ] **Step 1: Add documentation**

Create `docs/local-lite-build.md`:

````markdown
# Local-Lite Build

`local-lite` builds a native Trafodion core subset without Java, Maven, HDFS, HBase, Hive, DCS, REST, TrafCI, or Java client modules.

This target is a build-slimming milestone. It does not implement a local storage engine.

## Build

```bash
make local-lite
```

## Verification

```bash
make -n local-lite | egrep 'mvn|javac|hbase-trx|hbasetmlib2|hbase_utilities|JAVA_HOME'
```

Expected: no output.

```bash
find core/sqf/export -type f -perm -111 -print | xargs -r ldd | egrep 'libjvm|libhdfs'
```

Expected: no output.

## Disabled Runtime Behavior

HDFS, Hive, and HBase paths are intentionally disabled. If a local-lite binary reaches one of those paths, it must return an unsupported-operation error instead of silently succeeding.
````

- [ ] **Step 2: Run documented verification**

Run:

```bash
make -n local-lite | egrep 'mvn|javac|hbase-trx|hbasetmlib2|hbase_utilities|JAVA_HOME'
```

Expected: no output.

Run:

```bash
find core/sqf/export -type f -perm -111 -print | xargs -r ldd | egrep 'libjvm|libhdfs'
```

Expected: no output after a successful local-lite build.

- [ ] **Step 3: Commit docs**

```bash
git add docs/local-lite-build.md
git commit -m "docs: document local-lite build"
```

## Task 9: Final Regression Checks

**Files:**
- No planned edits.

- [ ] **Step 1: Confirm original target graph still contains full build modules**

Run:

```bash
make -n all | egrep 'jdbcT4|jdbc_type2|trafci|rest|dcs|hbase_utilities'
```

Expected: output still contains the original full-build modules. This confirms local-lite did not replace default behavior.

- [ ] **Step 2: Confirm local-lite excludes full build modules**

Run:

```bash
make -n local-lite | egrep 'mvn|javac|conn/jdbcT4|conn/jdbc_type2|conn/trafci|../dcs| rest|hbase_utilities|hbase-trx|hbasetmlib2|JAVA_HOME'
```

Expected: no output.

- [ ] **Step 3: Confirm compile and link**

Run:

```bash
make local-lite
```

Expected: command completes successfully.

- [ ] **Step 4: Confirm no Java/HDFS runtime dependencies**

Run:

```bash
find core/sqf/export -type f -perm -111 -print | xargs -r ldd | egrep 'libjvm|libhdfs'
```

Expected: no output.

- [ ] **Step 5: Capture final status**

Run:

```bash
git status --short
```

Expected: no uncommitted implementation changes.

## Self-Review

- Spec coverage: the plan adds `local-lite`, keeps default build unchanged, removes Java/Maven/HDFS/HBase/Hive from the local-lite graph, defines `TRAF_LOCAL_LITE`, adds explicit stub points, and includes compile/link/dependency verification.
- Red-flag scan: no incomplete markers or unspecified future work remains. The only adaptive work is constrained to symbols proven by compiler or linker output.
- Type consistency: the build variable is consistently `TRAF_LOCAL_LITE=1`; the make target is consistently `local-lite`; generated documentation uses the same names.
