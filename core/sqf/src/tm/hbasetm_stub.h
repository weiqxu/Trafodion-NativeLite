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

#ifndef HBASETM_STUB_H_
#define HBASETM_STUB_H_

#include <stddef.h>
#include <string.h>

#include "xa.h"

class CTmTimer;

enum HBASETM_TraceMask
{
   HBASETM_TraceOff = 0x0
};

enum HBTM_RetCode {
   RET_OK = 0,
   RET_NOTX,
   RET_READONLY,
   RET_ADD_PARAM,
   RET_EXCEPTION,
   RET_HASCONFLICT,
   RET_IOEXCEPTION,
   RET_NOCOMMITEX,
   RET_LAST
};

class HashMapArray
{
public:
   char* getRegionInfo(int64) { return emptyString(); }

private:
   static char* emptyString()
   {
      static char lv_empty[] = "";
      return lv_empty;
   }
};

class CHbaseTM
{
public:
   // Public member variables

private:
   // Private member variables
   bool iv_initialized;          // true after first call to HbaseTM_initialize
   int iv_my_nid;                // nid and pid of the process the library is running in.
   int iv_my_pid;                //

public:
   CHbaseTM() : iv_initialized(true), iv_my_nid(-1), iv_my_pid(-1) {};
   ~CHbaseTM() {};
   int initialize(short) { return RET_OK; }
   int initialize(HBASETM_TraceMask, bool, CTmTimer *, short) { return RET_OK; }
   bool Trace(HBASETM_TraceMask) { return false; }
   void setTrace(HBASETM_TraceMask) {}
   bool tm_stats() { return false; }
   int initJVM() { return RET_OK; }
   int detachThread() { return RET_OK; }
   bool initialized() { return iv_initialized; }
   CTmTimer *tmTimer() { return NULL; }
   void tmTimer(CTmTimer *) {}
      
   // Set/Get methods
   int my_nid() {return iv_my_nid;}
   void my_nid(int pv_nid) {iv_my_nid = pv_nid;}
   int my_pid() {return iv_my_pid;}
   void my_pid(int pv_pid) {iv_my_pid = pv_pid;}


   // TM interface methods
   short initConnection(short) { return RET_OK; }
   short beginTransaction(int64 *) { return RET_EXCEPTION; }
   short prepareCommit(int64, char *pp_errstr, int &pv_errstrlen)
   {
      return unavailable(pp_errstr, pv_errstrlen);
   }
   short doCommit(int64) { return RET_EXCEPTION; }
   short tryCommit(int64) { return RET_EXCEPTION; }
   short completeRequest(int64) { return RET_EXCEPTION; }
   short abortTransaction(int64) { return RET_EXCEPTION; }
   int dropTable(int64, const char*, int, char *pp_errstr, int &pv_errstrlen)
   {
      return unavailable(pp_errstr, pv_errstrlen);
   }
   int regTruncateOnAbort(int64, const char*, int, char *pp_errstr, int &pv_errstrlen)
   {
      return unavailable(pp_errstr, pv_errstrlen);
   }
   int createTable(int64, const char*, int, char**, int, int, char *pp_errstr, int &pv_errstrlen)
   {
      return unavailable(pp_errstr, pv_errstrlen);
   }
   int alterTable(int64, const char*, int, char **, int, int, char *pp_errstr, int &pv_errstrlen)
   {
      return unavailable(pp_errstr, pv_errstrlen);
   }
   int registerRegion(int64, const char [], const char [], int) {return RET_EXCEPTION; }
   int registerRegion(int64, int64, int, const char [], int, long, const char [], int)
   {
      return RET_EXCEPTION;
   }
   short addControlPoint() { return RET_OK; }
   int recoverRegion(int64 *, int64 *[], int64 *) { return RET_EXCEPTION; }
   short nodeDown(int) { return RET_OK; }
   short nodeUp(int) { return RET_OK; }

   int failedRegions(int64) { return 0; }
   int participatingRegions(int64) { return 0; }
   int unresolvedRegions(int64) {return 0; }
   short stall(int) { return RET_OK; }
   void shutdown() {}
   HashMapArray* requestRegionInfo() { return new HashMapArray(); }

private:
   // Private methods:
   void lock();
   void unlock();
   int setAndGetNid();
   static short unavailable(char *pp_errstr, int &pv_errstrlen)
   {
      static const char lc_msg[] = "storage unavailable in local-lite";
      int lv_copy_len = static_cast<int>(sizeof(lc_msg));
      if (pp_errstr && pv_errstrlen > 0)
      {
         if (lv_copy_len > pv_errstrlen)
            lv_copy_len = pv_errstrlen;
         memcpy(pp_errstr, lc_msg, lv_copy_len);
      }
      pv_errstrlen = lv_copy_len;
      return RET_EXCEPTION;
   }

}; // class CHbaseTM



extern CHbaseTM gv_HbaseTM;      // One global HbaseTM object

int HbaseTM_initialize(short pv_nid);
int HbaseTM_initialize(bool pp_tracing, bool pv_tm_stats, CTmTimer *pp_tmTimer, short pv_nid);
void HbaseTM_initiate_cp();
int HbaseTM_initiate_stall(int where);
HashMapArray* HbaseTM_process_request_regions_info();

#endif //HBASETM_H_
