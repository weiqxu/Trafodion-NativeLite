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

#ifndef LITE_STORAGE_H
#define LITE_STORAGE_H

#include <stdint.h>

#include <string>

// Backend-neutral storage contract for the Lite single-node runtime.
// SQL/catalog encodings remain above this boundary.  A backend sees one
// ordered key space, so catalog records, base rows, unique records, and index
// records can participate in the same transaction.

enum LiteStorageCode
{
  LITE_STORAGE_OK = 0,
  LITE_STORAGE_NOT_FOUND,
  LITE_STORAGE_CONFLICT,
  LITE_STORAGE_CANCELLED,
  LITE_STORAGE_NO_SPACE,
  LITE_STORAGE_CORRUPTION,
  LITE_STORAGE_INVALID,
  LITE_STORAGE_IO_ERROR
};

struct LiteStorageStatus
{
  LiteStorageStatus()
    : code(LITE_STORAGE_OK), retryable(false)
  {
  }

  bool ok() const { return code == LITE_STORAGE_OK; }

  LiteStorageCode code;
  bool retryable;
  std::string message;
};

struct LiteStorageOptions
{
  LiteStorageOptions()
    : createIfMissing(true), synchronousCommit(true),
      minimumFreeBytes(0), lockTimeoutMillis(5000)
  {
  }

  bool createIfMissing;
  bool synchronousCommit;
  uint64_t minimumFreeBytes;
  int64_t lockTimeoutMillis;
};

struct LiteStorageRecord
{
  std::string key;
  std::string value;
};

struct LiteStorageMetrics
{
  LiteStorageMetrics()
    : committedTransactions(0), rolledBackTransactions(0), conflicts(0),
      cancelledTransactions(0), bytesRead(0), bytesWritten(0),
      estimatedKeys(0), liveDataBytes(0)
  {
  }

  uint64_t committedTransactions;
  uint64_t rolledBackTransactions;
  uint64_t conflicts;
  uint64_t cancelledTransactions;
  uint64_t bytesRead;
  uint64_t bytesWritten;
  uint64_t estimatedKeys;
  uint64_t liveDataBytes;
};

class LiteStorageCursor
{
public:
  virtual ~LiteStorageCursor() {}

  // Returns one record at a time.  end is true only after the bounded range
  // is exhausted.  Implementations must never materialize the full range.
  virtual bool next(LiteStorageRecord *record,
                    bool *end,
                    LiteStorageStatus *status) = 0;
  virtual void cancel() = 0;
};

class LiteStorageTxn
{
public:
  virtual ~LiteStorageTxn() {}

  virtual bool get(const std::string &key,
                   std::string *value,
                   bool *found,
                   LiteStorageStatus *status) = 0;
  virtual bool put(const std::string &key,
                   const std::string &value,
                   LiteStorageStatus *status) = 0;
  virtual bool erase(const std::string &key,
                     LiteStorageStatus *status) = 0;
  virtual LiteStorageCursor *scan(const std::string &beginKey,
                                       const std::string &endKey,
                                       LiteStorageStatus *status) = 0;
  virtual bool commit(LiteStorageStatus *status) = 0;
  virtual bool rollback(LiteStorageStatus *status) = 0;
  virtual void cancel() = 0;
};

class LiteStorageSession
{
public:
  virtual ~LiteStorageSession() {}

  virtual LiteStorageTxn *begin(LiteStorageStatus *status) = 0;
};

class LiteStorageEngine
{
public:
  virtual ~LiteStorageEngine() {}

  virtual const char *name() const = 0;
  virtual bool open(const std::string &path,
                    const LiteStorageOptions &options,
                    LiteStorageStatus *status) = 0;
  virtual void close() = 0;
  virtual LiteStorageSession *createSession(
      LiteStorageStatus *status) = 0;
  virtual bool checkpoint(const std::string &path,
                          LiteStorageStatus *status) = 0;
  virtual bool backup(const std::string &path,
                      LiteStorageStatus *status) = 0;
  virtual bool restore(const std::string &backupPath,
                       const std::string &restorePath,
                       LiteStorageStatus *status) = 0;
  virtual bool verify(LiteStorageStatus *status) = 0;
  virtual LiteStorageMetrics metrics() const = 0;
};

// The M12 selected candidate.  Ownership is transferred to the caller.
LiteStorageEngine *LiteCreateRocksDBTransactionEngine();

// Independent transactional embedded candidate used by the M12B common
// correctness/recovery/workload gate.  Ownership is transferred to the caller.
LiteStorageEngine *LiteCreateSQLiteEngine();

#endif
