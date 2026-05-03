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

#ifndef LOCAL_LITE_CONFIG_H
#define LOCAL_LITE_CONFIG_H

#ifdef TRAF_LOCAL_LITE

// Initialize the local-lite runtime environment.
//
// Derives TRAF_HOME from the executable's own path (via /proc/self/exe),
// falling back to the $TRAF_HOME environment variable.  Sets TRAF_VAR,
// TRAF_CONF, and TRAF_LOG relative to TRAF_HOME.  Creates TRAF_VAR and
// TRAF_LOG directories if they don't exist.
//
// Must be called early in main(), before any code that reads these paths.
//
// Returns 0 on success, nonzero on failure.
int LocalLiteConfig_init();

#endif // TRAF_LOCAL_LITE

#endif // LOCAL_LITE_CONFIG_H
