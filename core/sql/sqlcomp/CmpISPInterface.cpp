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
 /* -*-C++-*-
******************************************************************************
*
* File:         CmpISPInterface.cpp
* Description:  
* Created:      3/26/2014 (relocate to this file)
* Language:     C++
*
*
*
*
******************************************************************************
*/

#include "CmpISPInterface.h"
#include "CmpISPStd.h"
#include "CmpStoredProc.h"
#include "QueryCacheSt.h"
#include "NATable.h"
#include "NATableSt.h"
#include "NARoutine.h"

#include <mutex>


CmpISPInterface::CmpISPInterface()
{
  initCalled_ = FALSE;
}

void CmpISPInterface::InitISPFuncs()
{
  static std::once_flag initOnce;
  std::call_once(initOnce, [this]() {
    SP_REGISTER_FUNCPTR regFunc = &(CmpISPFuncs::RegFuncs);

    // The stored-procedure registry is process-global and immutable after
    // startup.  Protect its first construction while allowing independent
    // embedded compiler contexts to initialize concurrently.
    QueryCacheStatStoredProcedure::Initialize(regFunc);
    QueryCacheEntriesStoredProcedure::Initialize(regFunc);
    QueryCacheDeleteStoredProcedure::Initialize(regFunc);

    HybridQueryCacheStatStoredProcedure::Initialize(regFunc);
    HybridQueryCacheEntriesStoredProcedure::Initialize(regFunc);

    NATableCacheStatStoredProcedure::Initialize(regFunc);
    NATableCacheEntriesStoredProcedure::Initialize(regFunc);
    NATableCacheDeleteStoredProcedure::Initialize(regFunc);

    NARoutineCacheStatStoredProcedure::Initialize(regFunc);
    NARoutineCacheDeleteStoredProcedure::Initialize(regFunc);

    CmpISPFuncs::procFuncsArray_.insert(CmpISPFuncs::ProcFuncsStruct());
    initCalled_ = TRUE;
  });
}

CmpISPInterface::~CmpISPInterface()
{
}

//
// NOTE: The cmpISPInterface variable remains process-global because the
//       registry is immutable after its once-only initialization.  The
//       initialization itself is protected by std::call_once so embedded
//       compiler instances may start on independent session threads.
//
CmpISPInterface cmpISPInterface;
