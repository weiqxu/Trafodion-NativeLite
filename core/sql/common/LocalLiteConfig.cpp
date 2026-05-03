/**********************************************************************
// @@@ START COPYRIGHT @@@
//
// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.
//
// @@@ END COPYRIGHT @@@
**********************************************************************/

#ifdef TRAF_LOCAL_LITE

#include <string>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

// Stub out ExSM symbols that are normally provided by libexecutor.so.
// In local-lite mode the IPC/messaging layer is never initialized, so
// these are never actually used — but the dynamic linker still needs
// to resolve them at load time.
__thread char ExSM_AssertBuf[32];

// Stubs for Seabed monitor functions.
// In local-lite mode there is no monitor process.  These functions are
// called by various parts of the runtime (NAMemory, SqlStats, etc.) but
// are never actually needed — the dynamic linker just needs to resolve
// them at load time and the runtime must not crash if they are called.
// Note: NOT extern "C" — seabed functions have C++ linkage.
#include <seabed/ms.h>
int msg_mon_get_my_segid(int *segid) { *segid = 0; return 0; }

// Helper: strip the last path component from a string in place.
// Returns a reference to the (modified) string for chaining.
static std::string& dirname(std::string &path)
{
    if (path.empty()) return path;
    // Remove trailing slashes.
    while (path.size() > 1 && path[path.size() - 1] == '/')
        path.resize(path.size() - 1);
    size_t pos = path.rfind('/');
    if (pos == std::string::npos) {
        path = ".";
    } else if (pos == 0) {
        path = "/";
    } else {
        path.resize(pos);
    }
    return path;
}

// Helper: create a directory if it doesn't already exist.
static int ensure_dir(const char *path)
{
    struct stat st;
    if (stat(path, &st) == 0) {
        return S_ISDIR(st.st_mode) ? 0 : -1;
    }
    if (mkdir(path, 0755) != 0) {
        return -1;
    }
    return 0;
}

int LocalLiteConfig_init()
{
    const char *trafHome = getenv("TRAF_HOME");
    std::string trafHomeBuf;

    // If TRAF_HOME is not already set, derive it from the executable path.
    if (!trafHome || !trafHome[0]) {
        char buf[4096];
        ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
        if (len <= 0) {
            return -1;
        }
        buf[len] = '\0';

        // The binary lives at <TRAF_HOME>/export/bin64d/<name>.
        // Go up 3 levels to find the install root.
        trafHomeBuf = buf;
        dirname(dirname(dirname(trafHomeBuf)));
        trafHome = trafHomeBuf.c_str();
    }

    // Set TRAF_HOME and derived paths as environment variables so
    // existing getenv() calls continue to work.
    setenv("TRAF_HOME", trafHome, 1);

    std::string trafVar  = std::string(trafHome) + "/var";
    std::string trafConf = std::string(trafHome) + "/conf";
    std::string trafLog  = trafVar + "/log";

    setenv("TRAF_VAR",  trafVar.c_str(),  1);
    setenv("TRAF_CONF", trafConf.c_str(), 1);
    setenv("TRAF_LOG",  trafLog.c_str(),  1);

    // Set default runtime configuration values that sqenvcom.sh and
    // sqgen normally provide.
    setenv("TRAF_CLUSTER_ID",  "1", 1);
    setenv("TRAF_INSTANCE_ID", "1", 1);
    setenv("TRAF_NODE_ID",     "0", 1);
    setenv("SQ_MON_CREATOR",   "0", 1);
    setenv("TRAFODION_VER",    "2.5.0", 1);

    // Create required directories.
    if (ensure_dir(trafVar.c_str()) != 0) {
        return -1;
    }
    if (ensure_dir(trafLog.c_str()) != 0) {
        return -1;
    }

    return 0;
}

#endif // TRAF_LOCAL_LITE
