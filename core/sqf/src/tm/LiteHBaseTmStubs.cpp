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

#include "hbasetm.h"

CHbaseTM gv_HbaseTM;

int HbaseTM_initialize(short pv_nid)
{
   return gv_HbaseTM.initialize(pv_nid);
}

int HbaseTM_initialize(bool pp_tracing, bool pv_tm_stats, CTmTimer *pp_tmTimer, short pv_nid)
{
   (void) pp_tracing;
   return gv_HbaseTM.initialize(HBASETM_TraceOff, pv_tm_stats, pp_tmTimer, pv_nid);
}

void HbaseTM_initiate_cp()
{
}

int HbaseTM_initiate_stall(int where)
{
   return gv_HbaseTM.stall(where);
}

HashMapArray* HbaseTM_process_request_regions_info()
{
   return gv_HbaseTM.requestRegionInfo();
}
