// @@@ START COPYRIGHT @@@
//
// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements. See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership. The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License. You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied. See the License for the
// specific language governing permissions and limitations
// under the License.
//
// @@@ END COPYRIGHT @@@

#ifndef LOCAL_LITE_STORAGE_MIGRATION_H
#define LOCAL_LITE_STORAGE_MIGRATION_H

#include "LocalLiteStorage.h"

#include <string>

// Copy the pre-M12 catalog + per-table RocksDB layout into the selected M12
// ordered key space. The source is read-only and remains the rollback window.
// The target transaction publishes its format marker only with every catalog,
// row, unique, and index record plus the integrity manifest.
bool LocalLiteMigrateLegacyStore(const std::string &legacyRoot,
                                 LocalLiteStorageEngine *target,
                                 LocalLiteStorageStatus *status);

bool LocalLiteVerifyMigratedStore(LocalLiteStorageEngine *target,
                                  LocalLiteStorageStatus *status);

#endif
